# local-services/tests/test_sub_bass_phase.py
"""Tests for kick vs bass sub-bass phase alignment."""
import numpy as np
import soundfile as sf

from khensu_services.analysis.sub_bass_phase import analyze_sub_bass_phase

SR = 22050


def _write(tmp_path, name, y, sr=SR):
    p = tmp_path / name
    sf.write(str(p), np.asarray(y, dtype=np.float32), sr)
    return str(p)


def _tone(freq, dur, phase=0.0, amp=0.6, sr=SR):
    t = np.arange(int(dur * sr)) / sr
    return amp * np.sin(2 * np.pi * freq * t + phase)


def test_in_phase_sub_reads_aligned(tmp_path):
    kick = _write(tmp_path, "kick.wav", _tone(60.0, 3.0, phase=0.0))
    bass = _write(tmp_path, "bass.wav", _tone(60.0, 3.0, phase=0.0))
    r = analyze_sub_bass_phase(kick, bass)
    assert r["correlation"] > 0.7
    assert abs(r["phase_degrees"]) < 30.0
    assert "Aligned" in r["verdict"]
    assert len(r["goniometer"]) > 0
    assert len(r["correlation_over_time"]) > 0


def test_anti_phase_sub_reads_cancelling(tmp_path):
    kick = _write(tmp_path, "kick.wav", _tone(60.0, 3.0, phase=0.0))
    bass = _write(tmp_path, "bass.wav", _tone(60.0, 3.0, phase=np.pi))  # 180deg
    r = analyze_sub_bass_phase(kick, bass)
    assert r["correlation"] < -0.7
    assert abs(r["phase_degrees"]) > 150.0
    assert "Out of phase" in r["verdict"]


def test_silent_bass_reports_insufficient(tmp_path):
    kick = _write(tmp_path, "kick.wav", _tone(60.0, 2.0))
    bass = _write(tmp_path, "bass.wav", np.zeros(int(2.0 * SR)))
    r = analyze_sub_bass_phase(kick, bass)
    assert "Insufficient" in r["verdict"]
    assert r["goniometer"] == []


# --- Nyquist safety (audit AUD-01) ------------------------------------------
# cutoff_hz reaches this analyzer straight off the request model, which sets no
# upper bound, so a cutoff at or above Nyquist is caller-reachable and used to
# raise ValueError out of sig.butter -> HTTP 500.


def test_cutoff_at_or_above_nyquist_is_clamped_not_fatal(tmp_path):
    kick = _write(tmp_path, "kick.wav", _tone(60.0, 3.0), sr=8000)
    bass = _write(tmp_path, "bass.wav", _tone(60.0, 3.0), sr=8000)
    r = analyze_sub_bass_phase(kick, bass, cutoff_hz=9000.0)  # well above 4 kHz Nyquist
    # The reported cutoff is the one actually filtered with, not the request.
    assert r["cutoff_hz"] < 4000.0
    assert np.isfinite(r["correlation"]) and np.isfinite(r["phase_degrees"])
    assert r["correlation"] > 0.7


def test_cutoff_below_nyquist_is_reported_unchanged(tmp_path):
    kick = _write(tmp_path, "kick.wav", _tone(60.0, 3.0))
    bass = _write(tmp_path, "bass.wav", _tone(60.0, 3.0))
    r = analyze_sub_bass_phase(kick, bass, cutoff_hz=120.0)
    assert r["cutoff_hz"] == 120.0


def test_non_positive_cutoff_is_not_fatal(tmp_path):
    kick = _write(tmp_path, "kick.wav", _tone(60.0, 3.0))
    bass = _write(tmp_path, "bass.wav", _tone(60.0, 3.0))
    r = analyze_sub_bass_phase(kick, bass, cutoff_hz=0.0)
    # Nothing passes the filter, so there is no sub energy left to judge.
    assert "Insufficient" in r["verdict"]
