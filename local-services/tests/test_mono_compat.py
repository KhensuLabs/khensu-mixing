# local-services/tests/test_mono_compat.py
"""Tests for cross-system mono-compatibility scoring."""
import numpy as np
import soundfile as sf

from khensu_services.analysis.mono_compat import analyze_mono_compat

SR = 44100


def _write(tmp_path, name, stereo):
    p = tmp_path / name
    sf.write(str(p), np.asarray(stereo, dtype=np.float32), SR)
    return str(p)


def _t(dur):
    return np.arange(int(dur * SR)) / SR


def test_correlated_mix_is_mono_safe(tmp_path):
    t = _t(2.0)
    m = 0.3 * np.sin(2 * np.pi * 80 * t) + 0.2 * np.sin(2 * np.pi * 2000 * t)
    path = _write(tmp_path, "corr.wav", np.stack([m, m], axis=1))
    r = analyze_mono_compat(path)
    by_id = {s["id"]: s for s in r["systems"]}
    assert by_id["phone"]["score"] > 90
    assert by_id["club_pa"]["score"] > 90


def test_antiphase_bass_cancels_on_club_and_vinyl(tmp_path):
    t = _t(2.0)
    bass = 0.5 * np.sin(2 * np.pi * 80 * t)
    highs = 0.2 * np.sin(2 * np.pi * 3000 * t)
    left = bass + highs
    right = -bass + highs          # bass hard anti-phase, highs correlated
    path = _write(tmp_path, "anti.wav", np.stack([left, right], axis=1))
    r = analyze_mono_compat(path)
    by_id = {s["id"]: s for s in r["systems"]}
    # Club PA / vinyl fold the low end -> big loss -> low score.
    assert by_id["club_pa"]["score"] < 40
    assert by_id["vinyl"]["score"] < 60
    # The worst band should be a low one.
    assert by_id["club_pa"]["worst_band_hz"][0] < 130


def test_mono_file_is_perfect(tmp_path):
    t = _t(1.5)
    m = 0.3 * np.sin(2 * np.pi * 200 * t)
    path = _write(tmp_path, "mono.wav", np.stack([m], axis=1))
    r = analyze_mono_compat(path)
    assert r["is_stereo"] is False
    assert all(s["score"] == 100 for s in r["systems"])
