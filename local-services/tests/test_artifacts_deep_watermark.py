# local-services/tests/test_artifacts_deep_watermark.py
"""Tests for the artifact deep tier (per-stem realism, spec §5.1 heavy pass)
and the watermark-presence detector."""
import numpy as np
import soundfile as sf

from khensu_services.analysis.artifacts import analyze_artifacts
from khensu_services.analysis.artifacts.deep_tier import (
    DrumTimingGridDetector, RepeatedHitVarianceDetector, VocalMicroPitchDetector,
)
from khensu_services.analysis.artifacts.watermark_tier import WatermarkPresenceDetector
from khensu_services.analysis.artifacts.base import AudioContext

SR = 22050


def _write(tmp_path, name, y, sr=SR):
    sf.write(str(tmp_path / name), y.astype(np.float32), sr)
    return str(tmp_path / name)


# ── Deep tier: drum timing grid ──────────────────────────────────────────

def _machine_drums(dur=8.0, jitter_ms=0.0, seed=1):
    rng = np.random.default_rng(seed)
    n = int(SR * dur)
    y = np.zeros(n)
    burst = int(SR * 0.05)
    tb = np.arange(burst) / SR
    hit = (np.sin(2 * np.pi * 180 * tb) + 0.6 * rng.uniform(-1, 1, burst)) * np.exp(-tb * 40)
    step = int(SR * 0.25)  # 16ths at 120 BPM-ish
    for k in range(0, n - burst, step):
        offs = int(rng.normal(0, jitter_ms * 1e-3 * SR)) if jitter_ms > 0 else 0
        i = np.clip(k + offs, 0, n - burst)
        y[i:i + burst] += hit
    return y


def test_drum_timing_grid_flags_machine_quantization(tmp_path):
    path = _write(tmp_path, "tight.wav", _machine_drums(jitter_ms=0.0))
    findings = DrumTimingGridDetector().scan_stem(path)
    assert any(f.kind == "drum_timing_grid" for f in findings)


def test_drum_timing_grid_passes_human_jitter(tmp_path):
    # ~12 ms of timing spread reads as a human drummer, not the grid.
    path = _write(tmp_path, "loose.wav", _machine_drums(jitter_ms=12.0, seed=7))
    findings = DrumTimingGridDetector().scan_stem(path)
    assert not findings


# ── Deep tier: repeated-hit timbre variance ─────────────────────────────

def test_repeated_hit_variance_flags_identical_samples(tmp_path):
    # Every hit is the SAME sample -> near-duplicate MFCC fingerprints.
    rng = np.random.default_rng(3)
    n = int(SR * 8.0)
    y = np.zeros(n)
    burst = int(SR * 0.06)
    tb = np.arange(burst) / SR
    sample = (np.sin(2 * np.pi * 200 * tb) + 0.5 * rng.uniform(-1, 1, burst)) * np.exp(-tb * 35)
    for k in range(0, n - burst, int(SR * 0.25)):
        y[k:k + burst] += sample
    path = _write(tmp_path, "sampled.wav", y)
    findings = RepeatedHitVarianceDetector().scan_stem(path)
    assert any(f.kind == "repeated_hit_variance" for f in findings)


# ── Deep tier: vocal micro-pitch ────────────────────────────────────────

def test_vocal_micro_pitch_flags_grid_locked(tmp_path):
    # Perfectly on-pitch sustained notes stepping through a scale = grid-locked.
    notes = [220.0, 246.94, 261.63, 293.66]  # A3 B3 C4 D4, exact ET
    seg = int(SR * 1.5)
    y = np.concatenate([
        0.3 * np.sin(2 * np.pi * f * np.arange(seg) / SR) for f in notes])
    path = _write(tmp_path, "tuned.wav", y)
    findings = VocalMicroPitchDetector().scan_stem(path)
    assert any(f.kind == "vocal_micro_pitch" for f in findings)


def test_vocal_micro_pitch_passes_drifting_voice(tmp_path):
    # A note with real drift/vibrato well off the grid should not flag.
    t = np.arange(int(SR * 6.0)) / SR
    drift = 30.0 * np.sin(2 * np.pi * 0.3 * t) + 15.0  # +15..45 cents wander
    f = 261.63 * 2 ** (drift / 1200.0)
    y = 0.3 * np.sin(2 * np.pi * np.cumsum(f) / SR)
    path = _write(tmp_path, "human.wav", y)
    findings = VocalMicroPitchDetector().scan_stem(path)
    assert not findings


# ── Watermark presence ──────────────────────────────────────────────────

def test_watermark_detector_ignores_natural_material():
    # Full-bandwidth music with a natural (deep) high-band floor.
    rng = np.random.default_rng(5)
    n = 44100 * 6
    t = np.arange(n) / 44100
    y = 0.3 * np.sin(2 * np.pi * 220 * t) + 0.1 * rng.standard_normal(n) * np.abs(np.sin(2 * np.pi * 2 * t))
    ctx = AudioContext(file_path="x", mono=y.astype(np.float32), sr=44100)
    assert WatermarkPresenceDetector().scan(ctx) == []


def test_watermark_detector_flags_stationary_high_band_carrier():
    # A persistent, noise-like 6.5-13.5 kHz carrier that never falls silent
    # (AID-03: tonal beds are now excluded as partials, so the carrier is
    # band-limited noise).
    rng = np.random.default_rng(9)
    sr = 44100
    n = sr * 6
    t = np.arange(n) / sr
    y = 0.3 * np.sin(2 * np.pi * 220 * t)                     # musical content
    spec = np.fft.rfft(rng.standard_normal(n))
    f = np.fft.rfftfreq(n, 1.0 / sr)
    spec[(f < 6500) | (f > 13500)] = 0.0
    carrier = np.fft.irfft(spec, n)
    carrier /= np.max(np.abs(carrier))
    y = y + 0.05 * carrier                                    # faint, constant
    ctx = AudioContext(file_path="x", mono=y.astype(np.float32), sr=sr)
    findings = WatermarkPresenceDetector().scan(ctx)
    assert any(f.kind == "watermark_presence" for f in findings)
    wm = findings[0]
    # AID-03: an undecoded heuristic never names a vendor - "other" <= 0.2,
    # and it is a zero-weight observation.
    assert "lyria" not in wm.services
    assert wm.services.get("other", 0) <= 0.2
    assert wm.evidence_class == "observation"
    assert "removal out of scope" in wm.evidence.lower()


def test_watermark_detector_ignores_sustained_harmonic_pad():
    # AID-03 repro: a 60 s organ chord used to read "Lyria 55%".
    sr = 44100
    t = np.arange(sr * 20) / sr
    y = np.zeros_like(t)
    for f0 in (130.81, 164.81, 196.0, 261.63):
        for h in range(1, 60):
            if f0 * h < 20000:
                y += np.sin(2 * np.pi * f0 * h * t) / h ** 0.7
    y /= np.max(np.abs(y)) * 2
    ctx = AudioContext(file_path="x", mono=y.astype(np.float32), sr=sr)
    assert WatermarkPresenceDetector().scan(ctx) == []


# ── Engine integration: stems trigger the deep tier ─────────────────────

def test_engine_runs_deep_tier_when_stems_supplied(tmp_path):
    mix = _write(tmp_path, "mix.wav", 0.3 * np.sin(2 * np.pi * 220 * np.arange(SR * 6) / SR))
    drums = _write(tmp_path, "drums.wav", _machine_drums(jitter_ms=0.0))

    without = analyze_artifacts(mix)
    with_stems = analyze_artifacts(mix, stems={"drums": drums})

    assert "drum_timing_grid" not in without["detectors_run"]
    assert "drum_timing_grid" in with_stems["detectors_run"]
    # The machine-tight drum stem should push at least one deep finding.
    assert any(f["detector"] == "drum_timing_grid" for f in with_stems["findings"])


def test_endpoint_accepts_stems(client, tmp_path):
    mix = _write(tmp_path, "m.wav", 0.3 * np.sin(2 * np.pi * 220 * np.arange(SR * 5) / SR))
    drums = _write(tmp_path, "d.wav", _machine_drums(jitter_ms=0.0))
    resp = client.post("/analyze/artifacts_scan",
                       json={"file_path": mix, "stems": {"drums": drums}})
    assert resp.status_code == 200
    assert "drum_timing_grid" in resp.json()["detectors_run"]
