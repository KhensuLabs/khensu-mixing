# local-services/tests/test_album_assembly.py
"""Tests for the Album Assembly consistency analyzer."""
import numpy as np
import soundfile as sf

from khensu_services.analysis.album_assembly import analyze_album

SR = 22050


def _track(tmp_path, name, amp=0.3, freq=300.0, dur=6.0):
    t = np.arange(int(dur * SR)) / SR
    # A little noise keeps the tilt fit stable across bands.
    rng = np.random.default_rng(hash(name) % 2**32)
    y = amp * np.sin(2 * np.pi * freq * t) + 0.01 * amp * rng.normal(0, 1, len(t))
    p = tmp_path / name
    sf.write(str(p), y.astype(np.float32), SR)
    return str(p)


def test_consistent_album_cohesive(tmp_path):
    paths = [_track(tmp_path, f"t{i}.wav", amp=0.3) for i in range(3)]
    result = analyze_album(paths)
    assert result["lufs_spread"] < 1.0
    assert not any(f["code"] == "loudness_jump" for f in result["flags"])


def test_loudness_jump_flagged(tmp_path):
    paths = [
        _track(tmp_path, "a.wav", amp=0.3),
        _track(tmp_path, "b.wav", amp=0.06),   # ~14 dB quieter
    ]
    result = analyze_album(paths)
    assert any(f["code"] == "loudness_jump" for f in result["flags"])
    assert result["lufs_spread"] > 2.0


def test_track_measurements_present(tmp_path):
    paths = [_track(tmp_path, "a.wav")]
    result = analyze_album(paths)
    t = result["tracks"][0]
    assert t["lufs"] < 0.0
    assert t["duration_sec"] > 5.0


def test_endpoint_happy_path(client, tmp_path):
    paths = [_track(tmp_path, f"t{i}.wav") for i in range(2)]
    resp = client.post("/analyze/album", json={"track_paths": paths})
    assert resp.status_code == 200
    body = resp.json()
    assert len(body["tracks"]) == 2
    assert "verdict" in body


def test_endpoint_missing_file_404(client):
    resp = client.post("/analyze/album", json={"track_paths": ["Z:/no.wav"]})
    assert resp.status_code == 404
