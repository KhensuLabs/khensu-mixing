from __future__ import annotations
import numpy as np
import pytest


@pytest.fixture
def kick_signal():
    """Synthetic kick-like signal: 60 Hz sine burst at 120 BPM intervals."""
    sr    = 22050
    total = 8.0  # seconds
    y     = np.zeros(int(sr * total), dtype=np.float32)
    bpm   = 120.0
    step  = int(sr * 60.0 / bpm)  # samples per beat
    burst = int(sr * 0.08)        # 80 ms burst per kick
    t_burst = np.linspace(0, 0.08, burst)
    envelope = np.exp(-t_burst * 30.0)  # fast decay
    tone     = np.sin(2 * np.pi * 60 * t_burst) * envelope
    for beat in range(int(bpm * total / 60)):
        i = beat * step
        end = min(i + burst, len(y))
        y[i:end] += (tone[: end - i] * 0.8).astype(np.float32)
    return np.stack([y, y]), sr


@pytest.fixture
def snare_signal():
    """Synthetic snare-like signal: 200 Hz + noise burst at 120 BPM on 2 and 4."""
    sr    = 22050
    total = 8.0
    y     = np.zeros(int(sr * total), dtype=np.float32)
    rng   = np.random.default_rng(42)
    burst = int(sr * 0.06)
    t_burst = np.linspace(0, 0.06, burst)
    envelope = np.exp(-t_burst * 40.0)
    tone     = (np.sin(2 * np.pi * 200 * t_burst) + rng.uniform(-1, 1, burst)) * envelope
    step  = int(sr * 60.0 / 120.0)  # beat step
    for hit in range(2, int(120.0 * total / 60), 2):  # beats 2, 4, 6 …
        i = hit * step
        end = min(i + burst, len(y))
        y[i:end] += (tone[: end - i] * 0.7).astype(np.float32)
    return np.stack([y, y]), sr


def test_drum_analyzer_returns_expected_keys(kick_signal):
    from khensu_services.analysis.drum_analyzer import analyze_drums
    y, sr = kick_signal
    result = analyze_drums(y, sr)
    assert "kick" in result
    assert "snare" in result
    assert "transient_sharpness" in result


def test_drum_analyzer_kick_count_plausible(kick_signal):
    from khensu_services.analysis.drum_analyzer import analyze_drums
    y, sr = kick_signal
    result = analyze_drums(y, sr)
    # 8 seconds at 120 BPM → 16 kicks expected; allow ±5 for onset detector margin
    assert 8 <= result["kick"]["count"] <= 24, (
        f"Expected ~16 kicks for 120 BPM / 8s signal, got {result['kick']['count']}"
    )


def test_drum_analyzer_kick_bpm_plausible(kick_signal):
    from khensu_services.analysis.drum_analyzer import analyze_drums
    y, sr = kick_signal
    result = analyze_drums(y, sr)
    if result["kick"]["count"] >= 4:
        avg_bpm = result["kick"]["avg_bpm"]
        # Allow 80–200 BPM — onset quantisation at 512-sample hop can shift IBIs
        assert 80.0 <= avg_bpm <= 200.0, f"kick avg_bpm implausible: {avg_bpm}"


def test_drum_analyzer_timing_regularity_high_for_steady_beat(kick_signal):
    from khensu_services.analysis.drum_analyzer import analyze_drums
    y, sr = kick_signal
    result = analyze_drums(y, sr)
    if result["kick"]["count"] >= 4:
        reg = result["kick"]["timing_regularity"]
        # Librosa onset quantisation at 512-sample hop introduces ~11ms jitter;
        # regularity > 0.5 is a realistic floor for a perfectly periodic synthetic signal.
        assert reg > 0.5, (
            f"Timing regularity should be high for a perfectly regular beat, got {reg:.3f}"
        )


def test_drum_analyzer_transient_sharpness_in_range(kick_signal):
    from khensu_services.analysis.drum_analyzer import analyze_drums
    y, sr = kick_signal
    result = analyze_drums(y, sr)
    assert 0.0 <= result["transient_sharpness"] <= 1.0


def test_drum_analyzer_kick_aggregate_fields(kick_signal):
    from khensu_services.analysis.drum_analyzer import analyze_drums
    y, sr = kick_signal
    result = analyze_drums(y, sr)
    kick = result["kick"]
    for field in ("count", "avg_bpm", "timing_regularity", "avg_body_db", "avg_fund_hz"):
        assert field in kick, f"Missing kick field: {field}"
    assert isinstance(kick["events"], list)


def test_drum_analyzer_event_fields(kick_signal):
    from khensu_services.analysis.drum_analyzer import analyze_drums
    y, sr = kick_signal
    result = analyze_drums(y, sr)
    for ev in result["kick"]["events"]:
        assert "onset_sec" in ev
        assert "attack_ms" in ev
        assert "body_db" in ev
        assert "fund_hz" in ev
        assert ev["onset_sec"] >= 0.0
        assert ev["attack_ms"] >= 0.0


def test_drum_analyzer_events_capped_at_20(kick_signal):
    from khensu_services.analysis.drum_analyzer import analyze_drums
    y, sr = kick_signal
    result = analyze_drums(y, sr)
    assert len(result["kick"]["events"]) <= 20
    assert len(result["snare"]["events"]) <= 20


# ---------------------------------------------------------------------------
# Full-mix regression (spec §2.1): the old mel-onset detector failed on dense
# mixes in BOTH directions — sustained bass/pads masked real 808 kicks, and
# (because librosa normalizes the onset envelope) a completely drumless steady
# signal produced dozens of noise onsets.
# ---------------------------------------------------------------------------

SR_MIX = 22050
DUR_MIX = 8.0


def _trap_mix():
    """Dense trap-style full mix: 808 kick + snare buried under a sustained
    55 Hz bassline, a synth pad chord, and a vibrato vocal tone."""
    n = int(SR_MIX * DUR_MIX)
    t = np.arange(n) / SR_MIX
    rng = np.random.default_rng(7)
    y = np.zeros(n)
    for f in (220.0, 277.2, 329.6):
        y += 0.12 * np.sin(2 * np.pi * f * t)          # pad
    y += 0.25 * np.sin(2 * np.pi * 55 * t)             # sustained sub bass
    y += 0.15 * np.sin(2 * np.pi * (440 + 8 * np.sin(2 * np.pi * 5 * t)) * t)

    bpm = 140.0
    step = int(SR_MIX * 60.0 / bpm)
    burst = int(SR_MIX * 0.25)
    tb = np.arange(burst) / SR_MIX
    kick = np.sin(2 * np.pi * (80 * np.exp(-tb * 8) + 45) * tb) * np.exp(-tb * 12)
    n_kicks = 0
    for beat in range(int(bpm * DUR_MIX / 60)):
        i = beat * step
        y[i:min(i + burst, n)] += 0.8 * kick[: min(i + burst, n) - i]
        n_kicks += 1

    sb = int(SR_MIX * 0.08)
    tsb = np.arange(sb) / SR_MIX
    snare = (np.sin(2 * np.pi * 200 * tsb)
             + 0.8 * rng.uniform(-1, 1, sb)) * np.exp(-tsb * 35)
    n_snares = 0
    for beat in range(1, int(bpm * DUR_MIX / 60), 2):
        i = beat * step
        y[i:min(i + sb, n)] += 0.5 * snare[: min(i + sb, n) - i]
        n_snares += 1

    y = (y / np.max(np.abs(y)) * 0.9).astype(np.float32)
    return np.stack([y, y]), n_kicks, n_snares


def _drumless_mix():
    """Pad + sustained bass + vocal tone, zero percussion."""
    n = int(SR_MIX * DUR_MIX)
    t = np.arange(n) / SR_MIX
    y = np.zeros(n)
    for f in (220.0, 277.2, 329.6):
        y += 0.2 * np.sin(2 * np.pi * f * t)
    y += 0.3 * np.sin(2 * np.pi * 55 * t)
    y += 0.2 * np.sin(2 * np.pi * 440 * t)
    y = (y / np.max(np.abs(y)) * 0.9).astype(np.float32)
    return np.stack([y, y])


def test_drum_analyzer_finds_808_kicks_in_dense_trap_mix():
    from khensu_services.analysis.drum_analyzer import analyze_drums
    y, n_kicks, n_snares = _trap_mix()
    result = analyze_drums(y, SR_MIX)
    # 18 kicks laid down; edge-frame guards may cost the first/last hit.
    assert n_kicks - 3 <= result["kick"]["count"] <= n_kicks + 4, (
        f"Expected ~{n_kicks} kicks in the trap mix, got {result['kick']['count']}"
    )
    assert n_snares - 3 <= result["snare"]["count"] <= n_snares + 4, (
        f"Expected ~{n_snares} snares in the trap mix, got {result['snare']['count']}"
    )


def test_drum_analyzer_silent_on_drumless_material():
    from khensu_services.analysis.drum_analyzer import analyze_drums
    result = analyze_drums(_drumless_mix(), SR_MIX)
    assert result["kick"]["count"] == 0, (
        f"Drumless signal produced {result['kick']['count']} phantom kicks"
    )
    assert result["snare"]["count"] == 0, (
        f"Drumless signal produced {result['snare']['count']} phantom snares"
    )
