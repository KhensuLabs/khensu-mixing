from __future__ import annotations

import numpy as np
import pytest
import soundfile as sf


@pytest.fixture
def test_tone_wav(tmp_path):
    """Stereo 6 s fixture: a bass tone, a mid tone and a little decorrelated
    noise, so every band of the extended analysis has energy. Generated here
    (the old tests/fixtures/test_tone.wav never existed, so this test was
    always skipped)."""
    sr = 44100
    t = np.arange(int(sr * 6.0)) / sr
    rng = np.random.default_rng(7)
    mono = 0.3 * np.sin(2 * np.pi * 55.0 * t) + 0.2 * np.sin(2 * np.pi * 440.0 * t)
    left = mono + 0.02 * rng.standard_normal(t.size)
    right = mono + 0.02 * rng.standard_normal(t.size)
    path = tmp_path / "test_tone.wav"
    sf.write(str(path), np.stack([left, right], axis=1).astype(np.float32), sr)
    return str(path)


def test_extended_endpoint_returns_extended_fields(client, test_tone_wav):
    resp = client.post("/analyze/extended", json={"file_path": test_tone_wav, "mode": "quick"})
    assert resp.status_code == 200, resp.text
    body = resp.json()
    assert "harshness_score" in body
    assert "mono_compat" in body
    assert "translation_risk" in body
    assert "resonance_hotspots" in body
    assert isinstance(body["mono_compat"], dict)
    assert "per_band_correlation" in body["mono_compat"]
    for band in ("sub", "bass", "midrange", "upper_mid", "air"):
        assert band in body["mono_compat"]["per_band_correlation"]
    for env in ("studio", "earbuds", "phone", "car", "club"):
        assert env in body["translation_risk"]


def test_extended_endpoint_rejects_missing_file(client):
    resp = client.post("/analyze/extended",
                       json={"file_path": "/nonexistent/file.wav", "mode": "quick"})
    assert resp.status_code in (400, 404, 422), resp.text
