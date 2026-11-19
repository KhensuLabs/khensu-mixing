# local-services/tests/test_perceptual_loudness.py
"""CE-AI-5: Moore-Glasberg loudness / partial loudness - maths anchors and the
/analyze/partial_loudness route contract.

Calibration convention under test: a full-scale sine plays at 100 dB SPL, and
the loudness constant is solved so a 1 kHz 40 dB SPL tone is exactly 1 sone."""
import numpy as np
import pytest
import soundfile as sf
from scipy.signal import butter, sosfiltfilt

from khensu_services.analysis import perceptual_loudness as pl

SR = 44100


def _amp(db_spl: float) -> float:
    return 10.0 ** ((db_spl - pl.DEFAULT_FULL_SCALE_SINE_DB_SPL) / 20.0)


def _tone(freq, db_spl, dur=2.0):
    t = np.arange(int(dur * SR)) / SR
    return _amp(db_spl) * np.sin(2 * np.pi * freq * t)


def _band_noise(lo, hi, db_spl, dur=2.0, seed=0):
    x = np.random.default_rng(seed).standard_normal(int(dur * SR))
    y = sosfiltfilt(butter(4, [lo, hi], btype="band", fs=SR, output="sos"), x)
    return y / np.sqrt(np.mean(y ** 2)) * _amp(db_spl) / np.sqrt(2.0)


def test_one_sone_at_40_phon_analytic_and_from_signal():
    assert pl.tone_loudness_sone(1000.0, 40.0) == pytest.approx(1.0, abs=1e-6)
    assert pl.loudness_of_signal(_tone(1000, 40), SR) == pytest.approx(1.0, abs=0.05)
    assert pl.sone_to_phon(1.0) == pytest.approx(40.0, abs=0.1)


def test_loudness_roughly_doubles_per_10_phon():
    s40, s50, s60, s80 = (pl.tone_loudness_sone(1000.0, lv) for lv in (40, 50, 60, 80))
    assert 1.7 < s50 / s40 < 2.4
    assert 3.4 < s60 < 4.8
    assert 12.0 < s80 < 20.0          # ANSI S3.4: 80 phon ~ 15-16 sone


def test_ear_sensitivity_shapes_loudness():
    # Same SPL: a 50 Hz tone is far quieter, a 4 kHz tone louder than 1 kHz.
    assert pl.tone_loudness_sone(50.0, 60.0) < 0.5 * pl.tone_loudness_sone(1000.0, 60.0)
    assert pl.tone_loudness_sone(4000.0, 60.0) > pl.tone_loudness_sone(1000.0, 60.0)


def test_spectral_summation():
    # The same total power split into two far-apart tones is louder.
    one = pl.loudness_of_signal(_tone(1000, 60), SR)
    two = pl.loudness_of_signal(_tone(500, 57) + _tone(4000, 57), SR)
    assert two > 1.5 * one


def test_partial_loudness_overlapping_vs_distant_masker():
    target = _tone(1000, 60)
    near = pl.partial_loudness(target, _band_noise(800, 1250, 70), SR)
    far = pl.partial_loudness(target, _band_noise(3500, 5000, 70), SR)
    quiet = pl.partial_loudness(target, np.zeros_like(target), SR)
    assert quiet["partial_ratio"] == pytest.approx(1.0, abs=0.01)
    assert far["partial_ratio"] > 0.9
    assert near["partial_ratio"] < 0.2
    assert near["verdict"] == "buried" and far["verdict"] == "clear"
    assert near["target_loudness_sone"] == pytest.approx(far["target_loudness_sone"], rel=1e-6)


def test_partial_loudness_rises_with_target_level():
    masker = _band_noise(300, 3000, 65)
    soft = pl.partial_loudness(_tone(1000, 60), masker, SR)["target_partial_loudness_sone"]
    loud = pl.partial_loudness(_tone(1000, 75), masker, SR)["target_partial_loudness_sone"]
    assert loud > soft


def _write(path, x):
    sf.write(str(path), np.asarray(x, dtype=np.float32).T if np.ndim(x) > 1 else x, SR)
    return str(path)


def test_route_mix_minus_target(client, tmp_path):
    vocal = _tone(1000, 70, dur=3.0)
    bed = _band_noise(800, 1250, 75, dur=3.0)
    v = _write(tmp_path / "vocals.wav", vocal)
    m = _write(tmp_path / "mix.wav", vocal + bed)
    r = client.post("/analyze/partial_loudness", json={"target_path": v, "mix_path": m})
    assert r.status_code == 200, r.text
    body = r.json()
    assert body["masker_source"] == "mix_minus_target"
    assert body["target_partial_loudness_sone"] < body["target_loudness_sone"]
    assert body["calibration"]["full_scale_sine_db_spl"] == 100.0
    assert body["bands"] and body["timeline"]
    assert body["target_share_db"] is not None


def test_route_explicit_maskers_and_region(client, tmp_path):
    v = _write(tmp_path / "v.wav", _tone(1000, 70, dur=3.0))
    d = _write(tmp_path / "d.wav", _band_noise(3000, 6000, 70, dur=3.0))
    r = client.post("/analyze/partial_loudness",
                    json={"target_path": v, "masker_paths": [d], "start_s": 0.5, "end_s": 2.5})
    assert r.status_code == 200, r.text
    body = r.json()
    assert body["masker_source"] == "stems"
    assert body["duration_s"] == pytest.approx(2.0, abs=0.01)
    assert body["timeline"][0]["start_s"] >= 0.5


def test_route_errors(client, tmp_path):
    v = _write(tmp_path / "v.wav", _tone(1000, 70, dur=1.0))
    assert client.post("/analyze/partial_loudness", json={"target_path": v}).status_code == 422
    assert client.post("/analyze/partial_loudness",
                       json={"target_path": str(tmp_path / "nope.wav"), "mix_path": v}).status_code == 404
    assert client.post("/analyze/partial_loudness",
                       json={"target_path": v, "mix_path": v, "start_s": 0.5, "end_s": 0.6}).status_code == 422
