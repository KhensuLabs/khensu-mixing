# local-services/tests/test_pitch_accuracy.py
"""Tests for the pYIN-based pitch-accuracy / tuning-map analyzer."""
import numpy as np

from khensu_services.analysis.pitch_accuracy import analyze_pitch_accuracy

SR = 22050


def _sine(freq_hz, dur=2.0, amp=0.6, sr=SR):
    t = np.arange(int(dur * sr)) / sr
    return (amp * np.sin(2 * np.pi * freq_hz * t)).astype(np.float64)


def test_in_tune_a4_detected_accurately():
    y = _sine(440.0)
    r = analyze_pitch_accuracy(y, SR)
    assert len(r["notes"]) >= 1
    assert r["notes"][0]["note_name"] == "A4"
    assert abs(r["notes"][0]["mean_cents_deviation"]) < 8.0
    assert r["mean_abs_cents"] < 8.0
    assert r["in_tune_pct"] == 100.0
    assert "excellent" in r["verdict"]


def test_sharp_note_flagged_with_correct_sign_and_magnitude():
    # +25 cents sharp of A4.
    sharp_freq = 440.0 * 2.0 ** (25.0 / 1200.0)
    y = _sine(sharp_freq)
    r = analyze_pitch_accuracy(y, SR)
    assert r["notes"][0]["note_name"] == "A4"
    assert r["notes"][0]["mean_cents_deviation"] > 15.0   # sharp = positive
    assert r["in_tune_pct"] == 0.0


def test_flat_note_flagged_with_negative_sign():
    flat_freq = 440.0 * 2.0 ** (-30.0 / 1200.0)
    y = _sine(flat_freq)
    r = analyze_pitch_accuracy(y, SR)
    assert r["notes"][0]["mean_cents_deviation"] < -15.0   # flat = negative


def test_worst_note_identifies_largest_deviation():
    # Two notes back to back: A4 in tune, then C5 sharp by 40 cents.
    sr = SR
    in_tune = _sine(440.0, dur=1.0)
    c5_sharp = _sine(523.25 * 2.0 ** (40.0 / 1200.0), dur=1.0)
    y = np.concatenate([in_tune, c5_sharp])
    r = analyze_pitch_accuracy(y, sr)
    assert r["worst_note"] is not None
    assert abs(r["worst_note"]["mean_cents_deviation"]) >= 15.0


def test_silence_returns_no_notes():
    y = np.zeros(SR * 2)
    r = analyze_pitch_accuracy(y, SR)
    assert r["notes"] == []
    assert r["verdict"] in ("no pitched content detected", "insufficient audio")


def test_too_short_audio_handled_gracefully():
    y = _sine(440.0, dur=0.05)
    r = analyze_pitch_accuracy(y, SR)
    assert r["verdict"] == "insufficient audio"
    assert r["notes"] == []


def test_no_notes_result_still_names_the_tuning_grid():
    # PitchAccuracyPanel shows "Measured against A=<grid>" from these keys;
    # the no-notes early returns used to drop them, so a requested A=432
    # grid read back as the 440 default (2026-10-16 contract check).
    sr = 22050
    silence = np.zeros(sr, dtype=np.float32)
    r = analyze_pitch_accuracy(silence, sr, reference_hz=432.0)
    assert r["notes"] == []
    assert r["measurement_reference_hz"] == 432.0
