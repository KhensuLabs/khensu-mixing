# local-services/tests/test_source_forensics.py
"""Tests for source-quality forensics."""
import numpy as np
import soundfile as sf
import scipy.signal as sig

from khensu_services.analysis.source_forensics import analyze_source_forensics

SR = 44100


def _write(tmp_path, name, y, sr=SR, subtype=None):
    p = tmp_path / name
    sf.write(str(p), np.asarray(y, dtype=np.float32), sr, subtype=subtype)
    return str(p)


def _fullband_noise(dur=3.0, sr=SR):
    rng = np.random.default_rng(0)
    m = 0.2 * rng.standard_normal(int(dur * sr))
    return np.stack([m, m], axis=1).astype(np.float32)


def test_brickwall_detected_on_lowpassed_source(tmp_path):
    # A steep elliptic low-pass at 16 kHz simulates an MP3-128 decode's cliff.
    y = _fullband_noise()
    sos = sig.ellip(12, 1.0, 90.0, 16000.0, btype="low", fs=SR, output="sos")
    lp = sig.sosfilt(sos, y[:, 0])
    stereo = np.stack([lp, lp], axis=1).astype(np.float32)
    path = _write(tmp_path, "mp3ish.wav", stereo)
    r = analyze_source_forensics(path)
    assert r["brickwall"]["brickwall_detected"] is True
    assert r["brickwall"]["bandwidth_hz"] < 18000
    assert "lossy" in r["brickwall"]["likely_source"]


def test_fullband_source_not_flagged(tmp_path):
    path = _write(tmp_path, "full.wav", _fullband_noise())
    r = analyze_source_forensics(path)
    assert r["brickwall"]["brickwall_detected"] is False


def test_fake_hi_res_flagged(tmp_path):
    # Genuinely upsample 44.1 kHz noise to 96 kHz — nothing exists above the
    # original 22.05 kHz Nyquist, the true signature of a fake hi-res file.
    rng = np.random.default_rng(1)
    base = 0.2 * rng.standard_normal(int(2.0 * SR))
    up = sig.resample_poly(base, 96000, SR)
    stereo = np.stack([up, up], axis=1).astype(np.float32)
    path = _write(tmp_path, "hires.wav", stereo, sr=96000)
    r = analyze_source_forensics(path)
    assert r["hi_res"]["applicable"] is True
    assert r["hi_res"]["upsampled_suspected"] is True
