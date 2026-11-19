"""Tests for /copilot/render_delta — the null-test (wet - dry) endpoint
behind the desktop app's Delta Solo feature."""
from __future__ import annotations

import numpy as np
import pytest
import soundfile as sf
from fastapi.testclient import TestClient

from khensu_services.server import app
from khensu_services.copilot.dsp import render_delta

client = TestClient(app)


@pytest.fixture
def tone_pair(tmp_path):
    sr = 44100
    t = np.arange(sr) / sr
    dry = (0.3 * np.sin(2 * np.pi * 440.0 * t)).astype(np.float32)
    wet = dry * 0.5   # a simple -6dB gain change
    dry_path = tmp_path / "dry.wav"
    wet_path = tmp_path / "wet.wav"
    sf.write(str(dry_path), dry, sr)
    sf.write(str(wet_path), wet, sr)
    return str(dry_path), str(wet_path)


def test_render_delta_identical_files_is_near_silent(tmp_path, tone_pair):
    dry_path, _ = tone_pair
    out_path = str(tmp_path / "delta.wav")
    result = render_delta(dry_path, dry_path, out_path)
    assert result["peak_delta_dbfs"] < -60.0

    y, _ = sf.read(out_path)
    assert np.max(np.abs(y)) < 1e-3


def test_render_delta_gain_change_is_audible(tmp_path, tone_pair):
    dry_path, wet_path = tone_pair
    out_path = str(tmp_path / "delta.wav")
    result = render_delta(dry_path, wet_path, out_path)
    assert result["peak_delta_dbfs"] > -20.0
    assert result["output_path"] == out_path

    import os
    assert os.path.isfile(out_path)


def test_render_delta_endpoint_roundtrip(tmp_path, tone_pair):
    dry_path, wet_path = tone_pair
    out_path = str(tmp_path / "delta_endpoint.wav")
    r = client.post("/copilot/render_delta", json={
        "dry_path": dry_path, "wet_path": wet_path, "output_path": out_path,
    })
    assert r.status_code == 200, r.text
    body = r.json()
    assert body["output_path"] == out_path
    assert body["peak_delta_dbfs"] > -20.0


def test_render_delta_endpoint_missing_file_404(tmp_path):
    r = client.post("/copilot/render_delta", json={
        "dry_path": "/nonexistent/a.wav", "wet_path": "/nonexistent/b.wav",
        "output_path": str(tmp_path / "out.wav"),
    })
    assert r.status_code == 404


def _write_float_wav_with_nan(path, sr=44100, n=1000, nan_index=500):
    y = np.zeros(n, dtype=np.float32)
    y[nan_index] = np.nan
    sf.write(str(path), y, sr, subtype="FLOAT")


def test_render_delta_rejects_non_finite_dry_source(tmp_path):
    """A corrupt dry source must fail closed, not silently produce a NaN
    delta that the peak-based clip-guard can't catch (NaN comparisons are
    always False) — audit CQ-01."""
    dry_path = tmp_path / "dry_nan.wav"
    wet_path = tmp_path / "wet.wav"
    _write_float_wav_with_nan(dry_path)
    sf.write(str(wet_path), np.zeros(1000, dtype=np.float32), 44100)

    with pytest.raises(ValueError, match="non-finite"):
        render_delta(str(dry_path), str(wet_path), str(tmp_path / "out.wav"))


def test_render_delta_rejects_non_finite_wet_source(tmp_path):
    dry_path = tmp_path / "dry.wav"
    wet_path = tmp_path / "wet_nan.wav"
    sf.write(str(dry_path), np.zeros(1000, dtype=np.float32), 44100)
    _write_float_wav_with_nan(wet_path)

    with pytest.raises(ValueError, match="non-finite"):
        render_delta(str(dry_path), str(wet_path), str(tmp_path / "out.wav"))
