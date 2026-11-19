# local-services/tests/test_album_normalization.py
"""Tests for the album back-to-back normalization simulator."""
from __future__ import annotations

import numpy as np
import soundfile as sf

from khensu_services.analysis import album_normalization as an

SR = 44100


def _track(tmp_path, name, amp, dur=2.0):
    t = np.arange(int(SR * dur)) / SR
    y = amp * np.sin(2 * np.pi * 220 * t)
    p = tmp_path / name
    sf.write(str(p), np.stack([y, y], axis=1).astype(np.float32), SR)
    return str(p)


def test_track_mode_flattens_album_mode_preserves(tmp_path):
    loud = _track(tmp_path, "loud.wav", amp=0.8)
    quiet = _track(tmp_path, "quiet.wav", amp=0.08)
    out = an.simulate_album_normalization([loud, quiet], "spotify")
    rows = out["tracks"]
    # Track mode pulls both toward the target -> equal post loudness (no jump).
    assert rows[0]["track_mode_post_lufs"] == rows[1]["track_mode_post_lufs"] == -14.0
    # Album mode preserves the loud->quiet drop -> a negative jump on track 2.
    assert rows[1]["album_mode_jump_lu"] < 0.0


def test_big_jump_flagged(tmp_path):
    loud = _track(tmp_path, "loud.wav", amp=0.9)
    quiet = _track(tmp_path, "quiet.wav", amp=0.03)
    out = an.simulate_album_normalization([loud, quiet], "spotify")
    assert out["summary"]["max_album_mode_jump_lu"] >= an.BIG_JUMP_LU
    assert out["summary"]["biggest_jump_notable"] is True


def test_platform_target_respected(tmp_path):
    t = _track(tmp_path, "t.wav", amp=0.3)
    out = an.simulate_album_normalization([t], "apple")
    assert out["target_lufs"] == -16.0
    assert out["tracks"][0]["track_mode_post_lufs"] == -16.0


def test_endpoint(client, tmp_path):
    a = _track(tmp_path, "a.wav", amp=0.5)
    b = _track(tmp_path, "b.wav", amp=0.2)
    resp = client.post("/analyze/album/normalization",
                       json={"track_paths": [a, b], "platform": "spotify"})
    assert resp.status_code == 200
    assert len(resp.json()["tracks"]) == 2


def test_endpoint_empty_422(client):
    resp = client.post("/analyze/album/normalization", json={"track_paths": []})
    assert resp.status_code == 422


def test_endpoint_missing_404(client):
    resp = client.post("/analyze/album/normalization",
                       json={"track_paths": ["/nope.wav"]})
    assert resp.status_code == 404
