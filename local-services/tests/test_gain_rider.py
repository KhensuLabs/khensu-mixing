# local-services/tests/test_gain_rider.py
"""Tests for the Smart Gain Rider (phrase-level pre-compression leveling)."""
import numpy as np
import soundfile as sf

from khensu_services.analysis.gain_rider import compute_gain_ride

SR = 22050


def _vocal_like(phrase_amps, phrase_sec=2.0, gap_sec=0.8, sr=SR):
    """Alternating 300 Hz 'phrases' at given amplitudes with silence gaps."""
    parts = []
    for amp in phrase_amps:
        t = np.arange(int(phrase_sec * sr)) / sr
        parts.append((amp * np.sin(2 * np.pi * 300.0 * t)).astype(np.float32))
        parts.append(np.zeros(int(gap_sec * sr), dtype=np.float32))
    return np.concatenate(parts)


def _write(tmp_path, name, y, sr=SR):
    p = tmp_path / name
    sf.write(str(p), y, sr)
    return str(p)


def test_detects_phrases(tmp_path):
    path = _write(tmp_path, "v.wav", _vocal_like([0.5, 0.05, 0.3]))
    result = compute_gain_ride(path)
    assert len(result["phrases"]) == 3


def test_loud_phrase_pulled_down_quiet_pushed_up(tmp_path):
    path = _write(tmp_path, "v.wav", _vocal_like([0.5, 0.06]))
    result = compute_gain_ride(path, target_rms_db=-18.0)
    loud, quiet = result["phrases"][0], result["phrases"][1]
    assert loud["gain_db"] < 0.0     # 0.5 amp sine RMS ≈ -9 dBFS -> pull down
    assert quiet["gain_db"] > 0.0    # 0.06 amp ≈ -27 dBFS -> push up (within ±12 clamp)
    # After correction each phrase should sit near the target.
    assert abs(loud["rms_db"] + loud["gain_db"] - (-18.0)) < 1.0
    assert abs(quiet["rms_db"] + quiet["gain_db"] - (-18.0)) < 1.0


def test_gain_clamped(tmp_path):
    path = _write(tmp_path, "v.wav", _vocal_like([0.9, 0.003]))
    result = compute_gain_ride(path, max_gain_db=6.0)
    for p in result["phrases"]:
        assert abs(p["gain_db"]) <= 6.0


def test_render_writes_file_and_levels(tmp_path):
    path = _write(tmp_path, "v.wav", _vocal_like([0.5, 0.05]))
    result = compute_gain_ride(path, render=True)
    assert result["rendered_path"] is not None
    y, _sr = sf.read(result["rendered_path"])
    assert len(y) > 0
    # Re-analyze the render: phrase levels should now be far more uniform.
    re = compute_gain_ride(result["rendered_path"])
    levels = [p["rms_db"] for p in re["phrases"]]
    assert max(levels) - min(levels) < 3.0


def test_endpoint_happy_path(client, tmp_path):
    path = _write(tmp_path, "v.wav", _vocal_like([0.4, 0.1]))
    resp = client.post("/analyze/gain_rider", json={"file_path": path})
    assert resp.status_code == 200
    body = resp.json()
    assert body["rendered_path"] is None
    assert len(body["phrases"]) == 2
    assert len(body["envelope_db"]) > 0


def test_endpoint_missing_file_404(client):
    resp = client.post("/analyze/gain_rider", json={"file_path": "Z:/nope.wav"})
    assert resp.status_code == 404
