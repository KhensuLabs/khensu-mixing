# local-services/tests/test_tuning_reference.py
"""Tests for tuning-reference detection."""
import numpy as np
import soundfile as sf

from khensu_services.analysis.tuning_reference import analyze_tuning_reference, _cents_to_ref_hz

SR = 44100


def _write(tmp_path, name, y):
    p = tmp_path / name
    sf.write(str(p), np.asarray(y, dtype=np.float32), SR)
    return str(p)


def _tone_scale(ref_a=440.0, dur=6.0):
    # A short major-triad arpeggio tuned to the given concert-A reference so
    # librosa has clear pitch content to estimate tuning from.
    semis = [0, 4, 7, 12, 7, 4]  # around A
    sig = np.zeros(int(dur * SR))
    seg = len(sig) // len(semis)
    for i, s in enumerate(semis):
        f = ref_a * (2.0 ** (s / 12.0))
        t = np.arange(seg) / SR
        sig[i * seg:(i + 1) * seg] = 0.3 * np.sin(2 * np.pi * f * t)
    return np.stack([sig, sig], axis=1)


def test_a440_detected():
    r = analyze_tuning_reference_helper(_tone_scale(440.0))
    assert abs(r["cents_from_440"]) < 15.0
    assert "440" in r["verdict"]


def test_a432_detected():
    r = analyze_tuning_reference_helper(_tone_scale(432.0))
    # 432 Hz is ~-31.8 cents from 440.
    assert r["cents_from_440"] < -15.0
    assert abs(r["reference_hz"] - 432.0) < 4.0


def _refhz():
    # Sanity for the pure conversion helper.
    assert abs(_cents_to_ref_hz(0.0) - 440.0) < 1e-6
    assert abs(_cents_to_ref_hz(-31.77) - 432.0) < 0.2


def analyze_tuning_reference_helper(stereo):
    import tempfile
    import os
    d = tempfile.mkdtemp()
    p = os.path.join(d, "t.wav")
    sf.write(p, np.asarray(stereo, dtype=np.float32), SR)
    return analyze_tuning_reference(p)


def test_conversion_helper():
    _refhz()
