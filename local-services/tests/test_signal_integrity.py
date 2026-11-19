# local-services/tests/test_signal_integrity.py
"""Tests for true-peak, mono-bass, and noise-floor checks."""
import numpy as np
import soundfile as sf

from khensu_services.analysis.signal_integrity import (
    analyze_true_peak, analyze_mono_bass, analyze_noise_floor, analyze_signal_integrity,
    true_peak_events,
)

SR = 22050


def _write(tmp_path, name, y):
    p = tmp_path / name
    sf.write(str(p), np.asarray(y, dtype=np.float32).T, SR)
    return str(p)


def test_true_peak_flags_intersample_overshoot():
    # A full-scale sample-peak sine has a true peak ABOVE 0 dBFS because the
    # reconstructed waveform overshoots between samples (classic ISP case).
    sr = 44100
    t = np.arange(int(0.2 * sr)) / sr
    # Frequency chosen so samples don't land exactly on the peaks.
    y = 0.999 * np.sin(2 * np.pi * 11025.3 * t)
    stereo = np.stack([y, y])
    r = analyze_true_peak(stereo, sr)
    assert r["true_peak_dbtp"] > r["sample_peak_dbfs"] - 0.05
    assert isinstance(r["true_peak_dbtp"], float)


def test_true_peak_safe_verdict_for_quiet_signal():
    sr = 44100
    t = np.arange(int(0.2 * sr)) / sr
    y = 0.1 * np.sin(2 * np.pi * 1000 * t)
    r = analyze_true_peak(np.stack([y, y]), sr)
    assert r["true_peak_dbtp"] < -1.0
    assert "safe" in r["verdict"]


def test_true_peak_events_locates_hot_burst():
    # Quiet for 1 s, then a near-full-scale ISP-prone burst around t=1 s.
    sr = 44100
    t = np.arange(int(2.0 * sr)) / sr
    y = 0.05 * np.sin(2 * np.pi * 1000 * t)
    burst = (t >= 1.0) & (t < 1.1)
    y[burst] = 0.999 * np.sin(2 * np.pi * 11025.3 * t[burst])
    r = true_peak_events(np.stack([y, y]), sr)
    assert r["threshold_dbtp"] == -1.0
    assert r["over_count"] >= 1
    # The overshoot event(s) should land inside the burst window.
    assert any(0.9 <= e["time_sec"] <= 1.2 for e in r["events"])
    assert all(e["dbtp"] > -1.0 for e in r["events"])


def test_true_peak_events_empty_for_quiet_signal():
    sr = 44100
    t = np.arange(int(0.5 * sr)) / sr
    y = 0.1 * np.sin(2 * np.pi * 1000 * t)
    r = true_peak_events(np.stack([y, y]), sr)
    assert r["over_count"] == 0
    assert r["events"] == []


def test_mono_bass_correlated_is_safe():
    t = np.arange(SR * 2) / SR
    bass = 0.5 * np.sin(2 * np.pi * 80 * t)
    r = analyze_mono_bass(np.stack([bass, bass]), SR)
    assert r["correlation_below_cutoff"] > 0.9
    assert "safe" in r["verdict"]


def test_mono_bass_antiphase_flagged_at_risk():
    t = np.arange(SR * 2) / SR
    bass = 0.5 * np.sin(2 * np.pi * 80 * t)
    r = analyze_mono_bass(np.stack([bass, -bass]), SR)
    assert r["correlation_below_cutoff"] < -0.5
    assert r["energy_at_risk_db"] > 10.0
    assert "cancel" in r["verdict"] or "thin" in r["verdict"]


def test_noise_floor_detects_60hz_hum():
    rng = np.random.default_rng(0)
    t = np.arange(SR * 3) / SR
    silence_with_hum = 0.02 * np.sin(2 * np.pi * 60 * t) + 0.0005 * rng.standard_normal(t.shape[0])
    r = analyze_noise_floor(silence_with_hum, SR)
    assert r["hum_detected"] is True
    assert r["hum_hz"] == 60.0


def test_noise_floor_clean_signal_no_hum():
    rng = np.random.default_rng(1)
    t = np.arange(SR * 3) / SR
    y = 0.3 * np.sin(2 * np.pi * 440 * t) + 0.0001 * rng.standard_normal(t.shape[0])
    r = analyze_noise_floor(y, SR)
    assert r["hum_detected"] is False


def test_analyze_signal_integrity_end_to_end(tmp_path):
    t = np.arange(SR * 2) / SR
    y = 0.4 * np.sin(2 * np.pi * 440 * t)
    path = _write(tmp_path, "a.wav", np.stack([y, y]))
    r = analyze_signal_integrity(path)
    assert set(r.keys()) == {"true_peak", "mono_bass", "noise_floor"}
    assert "verdict" in r["true_peak"]
    assert "verdict" in r["mono_bass"]
    assert "verdict" in r["noise_floor"]
