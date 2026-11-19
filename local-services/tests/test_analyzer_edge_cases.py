"""Low-severity audit fixes (2026-10-11 batch)."""
from __future__ import annotations

import json
from pathlib import Path

import numpy as np
import pytest

SR = 22050


# --- mood / energy / memorability -------------------------------------------

def test_unknown_key_is_neutral_mood_not_minor():
    from khensu_services.analysis.mood_energy import estimate_mood

    assert estimate_mood("unknown", 80.0, 140.0) == "neutral"
    assert estimate_mood("", 10.0, 70.0) == "neutral"
    assert estimate_mood("A minor", 10.0, 70.0) == "melancholic"
    assert estimate_mood("C major", 80.0, 130.0) == "euphoric"


def test_arousal_uses_folded_bpm():
    from khensu_services.analysis.mood_energy import estimate_mood

    # A 240 BPM double-time read of a 120 BPM groove is not "faster" than 120.
    assert estimate_mood("C major", 10.0, 240.0) == estimate_mood("C major", 10.0, 120.0)


def test_energy_tempo_term_uses_folded_bpm():
    from khensu_services.analysis.mood_energy import estimate_energy

    y = (0.1 * np.sin(2 * np.pi * 220 * np.arange(SR) / SR)).astype(np.float32)
    assert estimate_energy(y, SR, 45.0) == pytest.approx(estimate_energy(y, SR, 90.0))


def test_memorability_bpm_alignment_is_octave_folded():
    from khensu_services.analysis.memorability_analyzer import score_memorability

    hooks = {"hook_strength": 0.5, "hook_instances": []}
    secs = [{"label": "A"}, {"label": "B"}]
    assert score_memorability(hooks, secs, 240.0) == score_memorability(hooks, secs, 120.0)


# --- genre ------------------------------------------------------------------

def test_genre_targets_corrected_tempo_ranges():
    p = Path(__file__).resolve().parents[2] / "resources" / "genre_norms" / "genre_targets.json"
    by_id = {g["id"]: g for g in json.loads(p.read_text(encoding="utf-8"))["genres"]}
    assert by_id["crunk"]["typical_bpm_range"] == [75, 105]
    assert by_id["hyphy"]["typical_bpm_range"] == [90, 110]
    assert by_id["melodic-rap"]["typical_bpm_range"] == [80, 160]
    assert by_id["dembow"]["typical_bpm_range"] == [110, 130]
    assert by_id["grime"]["typical_bpm_range"] == [138, 142]


def test_half_time_trap_read_is_trap_not_lofi():
    from khensu_services.analysis.genre_detector import estimate_genre_and_subgenre

    t = np.arange(SR * 4) / SR
    sub = 0.6 * np.sin(2 * np.pi * 45 * t)           # 808-heavy, dark
    assert estimate_genre_and_subgenre(sub.astype(np.float32), SR, 72.0) == ("hiphop", "trap")


def test_genre_blend_low_end_uses_welch_and_matches_fft():
    from khensu_services.analysis.dsp_metrics import low_band_energy_ratio

    rng = np.random.default_rng(0)
    t = np.arange(SR * 3) / SR
    y = 0.5 * np.sin(2 * np.pi * 60 * t) + 0.1 * rng.standard_normal(t.size)
    fft = np.abs(np.fft.rfft(y)) ** 2
    freqs = np.fft.rfftfreq(len(y), 1.0 / SR)
    exact = float(np.sum(fft[freqs < 250.0]) / np.sum(fft))
    assert low_band_energy_ratio(y, SR, 250.0) == pytest.approx(exact, abs=0.03)
    assert low_band_energy_ratio(np.zeros(10), SR) == 0.0


# --- meter -------------------------------------------------------------------

def test_bar_similarity_compares_equal_bar_counts():
    from khensu_services.analysis._shared_analysis import _harmonic_bar_similarities

    chroma = np.random.default_rng(1).random((12, 30))   # 29 beats: 8 bars of 4 don't fit
    h4, h3 = _harmonic_bar_similarities(chroma)
    assert h4 is not None and h3 is not None
    # With bars (2, 4) only, both use the same number of lags.
    from khensu_services.analysis._shared_analysis import _harmonic_bar_similarity
    assert h3 == pytest.approx(_harmonic_bar_similarity(chroma, (6, 12)))
    assert _harmonic_bar_similarities(np.random.random((12, 8))) == (None, None)


def test_felt_bpm_only_for_six_eight_at_eighth_pulse():
    from khensu_services.analysis.advanced_analyzer import _felt_bpm

    beats = list(np.arange(0, 10, 60.0 / 180.0))           # 180 BPM eighth pulse
    assert _felt_bpm({"time_signature": "6/8", "beats_per_bar": 6}, beats, 180.0) == pytest.approx(60.0, abs=0.5)
    assert _felt_bpm({"time_signature": "6/8", "beats_per_bar": 2}, beats, 60.0) is None
    assert _felt_bpm({"time_signature": "4/4", "beats_per_bar": 4}, beats, 180.0) is None


# --- MIDI extraction / alignment / separator ----------------------------------

def test_final_pitched_note_is_not_one_hop_short(monkeypatch):
    from khensu_services.analysis import midi_extract

    hop = 512
    n_frames = 40
    f0 = np.full(n_frames, 440.0)
    # The tracker (NEW-7: CREPE or pyin) is replaced by a constant A4.
    from khensu_services.analysis import vocal_pitch
    monkeypatch.setattr(vocal_pitch, "track", lambda *a, **k: (f0, np.ones(n_frames, bool), np.ones(n_frames)))
    y = np.zeros(hop * (n_frames - 1), dtype=np.float32) + 0.1
    res = midi_extract.extract_pitched(y, SR, hop=hop)
    assert res["notes"]
    assert res["notes"][-1]["end_sec"] == pytest.approx(n_frames * hop / SR, abs=1e-3)


def test_take_alignment_drops_onsets_that_cannot_fit(monkeypatch):
    from khensu_services.analysis import take_alignment as ta

    n = SR
    y = np.zeros((1, n), dtype=np.float32)
    monkeypatch.setattr(ta, "_load", lambda p: (y, SR))
    monkeypatch.setattr(ta, "detect_onsets", lambda mono, sr: np.array([0.5, 0.9, 0.95]))
    monkeypatch.setattr(ta, "_target_offsets",
                        lambda onsets, *a, **k: [10.0] + [0.0] * (len(onsets) - 1))
    # The first boundary is pushed to the last legal position (n - 2*xf); the
    # later onsets then have no room and are dropped (the old clamp placed
    # them past n - 2*xf).
    res = ta._process_take("x.wav", "align", np.array([]), 0.1, 0.0, 1.0, 100.0, None)
    xf = max(1, int(round(ta._XFADE_SEC * SR)))
    assert len(res["segments"]) == 1
    seg = res["segments"][0]
    assert seg["start_sec"] * SR + seg["shift_ms"] * 1e-3 * SR <= n - 2 * xf + 1


def test_separation_quality_cap_uses_sample_rate():
    from khensu_services.analysis.separator import _compute_separation_quality

    rng = np.random.default_rng(0)
    sr = 8000
    h = rng.standard_normal(sr * 20)
    p = np.concatenate([rng.standard_normal(sr * 10), h[sr * 10:]])   # identical after 10 s
    # Only the first 10 s (at 8 kHz) are evaluated: independent -> high score.
    assert _compute_separation_quality(h, p, sr) > 0.8


# --- text factory ---------------------------------------------------------------

def test_text_factory_advice_corrections():
    from khensu_services.recommendations import text_factory as tf

    # ENG-6: the templates live in templates_*.py beside text_factory.py.
    blob = "".join(p.read_text(encoding="utf-8")
                   for p in sorted(Path(tf.__file__).parent.glob("t*.py")))
    assert "high-pass the side channel" in blob and "low-pass the side channel" not in blob
    assert "parallel compression raises density and integrated LUFS" in blob
    assert "no false positives" not in blob
    assert "EBU R128 methodology" not in blob and "in LU" in blob
    assert "cross-channel RMS ratio" not in blob
    assert "above 0.3 on the full mix in mono" not in blob
    assert "set lookahead to 1 ms to preserve transients" not in blob
    assert "which can cause pumping" not in blob
