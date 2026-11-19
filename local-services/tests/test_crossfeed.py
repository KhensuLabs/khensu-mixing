# local-services/tests/test_crossfeed.py
"""Tests for the headphone-vs-speaker crossfeed A/B."""
from __future__ import annotations

import os

import numpy as np
import soundfile as sf

from khensu_services.copilot import crossfeed as cf

SR = 44100


def _wide(tmp_path, name, width=1.0, dur=1.0):
    t = np.arange(int(SR * dur)) / SR
    mid = 0.3 * np.sin(2 * np.pi * 220 * t)
    side = width * 0.3 * np.sin(2 * np.pi * 3000 * t)
    left = mid + side
    right = mid - side
    p = tmp_path / name
    sf.write(str(p), np.stack([left, right], axis=1).astype(np.float32), SR)
    return str(p)


def test_renders_both_ab_files(tmp_path):
    p = _wide(tmp_path, "m.wav")
    out = cf.render_crossfeed_ab(p, str(tmp_path / "out"))
    assert os.path.isfile(out["headphone_path"])
    assert os.path.isfile(out["speaker_path"])


def test_speaker_image_narrows_width(tmp_path):
    p = _wide(tmp_path, "wide.wav", width=1.5)
    out = cf.render_crossfeed_ab(p, str(tmp_path / "out"))
    # Crossfeed pulls the image in, so speaker width <= headphone width.
    assert out["width_speaker"] <= out["width_headphone"]
    assert out["width_reduction_pct"] >= 0.0


def test_metrics_present(tmp_path):
    p = _wide(tmp_path, "m.wav")
    out = cf.render_crossfeed_ab(p, str(tmp_path / "out"))
    for k in ("width_headphone", "width_speaker", "width_reduction_pct",
              "correlation_headphone", "correlation_speaker", "note"):
        assert k in out


def test_endpoint(client, tmp_path):
    p = _wide(tmp_path, "song.wav")
    resp = client.post("/copilot/crossfeed_ab",
                       json={"file_path": p, "output_dir": str(tmp_path / "o")})
    assert resp.status_code == 200
    assert "speaker_path" in resp.json()


def test_endpoint_404(client):
    resp = client.post("/copilot/crossfeed_ab", json={"file_path": "/nope.wav"})
    assert resp.status_code == 404
