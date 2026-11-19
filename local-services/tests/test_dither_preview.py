# local-services/tests/test_dither_preview.py
"""Tests for the dither & noise-shaping preview."""
from __future__ import annotations

import numpy as np
import soundfile as sf

from khensu_services.analysis import dither_preview as dp

SR = 44100


def _quiet_tone(dur=1.0, amp=0.02, f=440.0):
    # A low-level tone so 16-bit quantisation error is meaningful.
    t = np.arange(int(SR * dur)) / SR
    y = amp * np.sin(2 * np.pi * f * t)
    return np.stack([y, y], axis=1).astype(np.float64)


def test_truncate_mode_actually_truncates_not_rounds():
    """backend-review-2026-07-17.md P2: the 'truncate' mode used np.round,
    so it silently rounded to nearest. Real two's-complement truncation
    (dropping low bits) rounds toward -inf (floor), not toward zero."""
    rng = np.random.default_rng(0)
    # 8-bit -> q = 128. 0.005 * 128 = 0.64: floor drops to 0.0 (rounding
    # would give 1/128); -0.64 floors to -1 LSB (np.trunc would give 0.0).
    y = np.array([0.005, -0.005])
    out = dp._quantize_vectorised(y, bits=8, dither=False, rng=rng, truncate=True)
    assert out[0] == 0.0
    assert out[1] == -1.0 / 128.0


def test_truncation_error_is_more_correlated_than_dither(tmp_path):
    p = tmp_path / "m.wav"
    sf.write(str(p), _quiet_tone(), SR, subtype="FLOAT")
    out = dp.analyze_dither(str(p), target_bits=8, out_dir=str(tmp_path / "d"))
    modes = {m["mode"]: m for m in out["modes"]}
    # Truncation error correlates with the signal; TPDF dither decorrelates it.
    assert modes["truncate"]["distortion_correlation"] >= modes["tpdf"]["distortion_correlation"]


def test_all_three_modes_present_with_metrics(tmp_path):
    p = tmp_path / "m.wav"
    sf.write(str(p), _quiet_tone(), SR, subtype="FLOAT")
    out = dp.analyze_dither(str(p), target_bits=16, out_dir=str(tmp_path / "d"))
    assert [m["mode"] for m in out["modes"]] == ["truncate", "tpdf", "shaped"]
    for m in out["modes"]:
        assert "noise_floor_dbfs" in m and "distortion_correlation" in m
    assert "noise_floor_delta_db" in out


def test_audition_render_written_at_target_depth(tmp_path):
    p = tmp_path / "m.wav"
    sf.write(str(p), _quiet_tone(), SR, subtype="FLOAT")
    out = dp.analyze_dither(str(p), target_bits=16, out_dir=str(tmp_path / "d"))
    import os
    assert os.path.isfile(out["audition_path"])
    info = sf.info(out["audition_path"])
    assert "16" in info.subtype  # PCM_16


def test_endpoint_missing_file_404(client):
    resp = client.post("/analyze/dither/preview", json={"file_path": "/nope.wav"})
    assert resp.status_code == 404


def test_endpoint_bad_bits_422(client, tmp_path):
    p = tmp_path / "m.wav"
    sf.write(str(p), _quiet_tone(), SR, subtype="FLOAT")
    resp = client.post("/analyze/dither/preview",
                       json={"file_path": str(p), "target_bits": 20})
    assert resp.status_code == 422


def test_endpoint_runs(client, tmp_path):
    p = tmp_path / "song.wav"
    sf.write(str(p), _quiet_tone(), SR, subtype="FLOAT")
    resp = client.post("/analyze/dither/preview",
                       json={"file_path": str(p), "target_bits": 16,
                             "output_dir": str(tmp_path / "out")})
    assert resp.status_code == 200
    assert resp.json()["target_bits"] == 16


def test_shaped_dither_noise_is_highpass_shaped():
    # AUDIT-082: the fed-back error includes the dither, so the WHOLE output
    # error (dither + rounding) is shaped by 1 - z^-1: little energy at low
    # frequencies, most of it near Nyquist. Feeding back only the rounding
    # error left the TPDF dither white (flat spectrum).
    rng = np.random.default_rng(1)
    y = _quiet_tone(dur=2.0)[:, :1]
    err = (dp._quantize_shaped(y, 16, rng) - y)[:, 0]
    spec = np.abs(np.fft.rfft(err)) ** 2
    f = np.fft.rfftfreq(err.size, 1.0 / SR)
    low = spec[(f > 20) & (f < 2000)].mean()
    high = spec[(f > 18000) & (f < 22000)].mean()
    # |1 - e^-jw|^2 is ~0.008 at 2 kHz vs ~3.9 near 20 kHz: > 20 dB apart.
    assert 10 * np.log10(high / low) > 20.0


def test_no_dither_recommended_when_target_not_below_source(tmp_path):
    p = tmp_path / "src16.wav"
    sf.write(str(p), _quiet_tone(), SR, subtype="PCM_16")
    for bits, needed in ((24, False), (16, False), (8, True)):
        r = dp.analyze_dither(str(p), target_bits=bits, out_dir=str(tmp_path / f"o{bits}"))
        assert r["source_bits"] == 16
        assert r["dither_needed"] is needed
        assert ("No dither needed" in r["recommendation"]) is (not needed)
    pf = tmp_path / "srcf.wav"
    sf.write(str(pf), _quiet_tone(), SR, subtype="FLOAT")
    r = dp.analyze_dither(str(pf), target_bits=24, out_dir=str(tmp_path / "of"))
    assert r["source_bits"] is None and r["dither_needed"] is True
