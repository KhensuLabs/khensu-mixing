# local-services/tests/test_ab_critique.py
"""Tests for the conversational A/B critique (measurement-grounded)."""
from __future__ import annotations

import numpy as np
import soundfile as sf

from khensu_services.copilot import ab_critique as abc

SR = 44100


def _write(tmp_path, name, amp=0.3, clip=False, dynamic=True, dur=2.0):
    t = np.arange(int(SR * dur)) / SR
    y = amp * np.sin(2 * np.pi * 220 * t)
    if dynamic:
        y *= (0.3 + 0.7 * np.abs(np.sin(2 * np.pi * 0.5 * t)))  # amplitude envelope
    if clip:
        y = np.clip(y * 6.0, -1.0, 1.0)   # genuinely drives into the rails
    p = tmp_path / name
    sf.write(str(p), np.stack([y, y], axis=1).astype(np.float32), SR)
    return str(p)


def test_clipped_take_loses_on_clipping(tmp_path):
    clean = _write(tmp_path, "clean.wav", amp=0.3, clip=False)
    clipped = _write(tmp_path, "clipped.wav", amp=0.3, clip=True)
    out = abc.critique_ab(clean, clipped, "A", "B")
    clip_cmp = next(c for c in out["comparisons"] if c["metric"] == "clip_samples")
    assert clip_cmp["winner"] == "A"   # clean take wins the clipping dimension


def test_matched_loudness_gain_reported(tmp_path):
    loud = _write(tmp_path, "loud.wav", amp=0.6)
    quiet = _write(tmp_path, "quiet.wav", amp=0.15)
    out = abc.critique_ab(loud, quiet)
    # B (quiet) is gained up to match A (loud): positive match gain.
    assert out["loudness_match_gain_db"] > 0.0


def test_identical_takes_tie(tmp_path):
    p = _write(tmp_path, "same.wav")
    out = abc.critique_ab(p, p)
    assert out["overall"] == "tie"
    assert all(c["winner"] == "tie" for c in out["comparisons"])


def test_verdict_and_dimensions_present(tmp_path):
    a = _write(tmp_path, "a.wav", amp=0.3)
    b = _write(tmp_path, "b.wav", amp=0.3, clip=True)
    out = abc.critique_ab(a, b)
    assert isinstance(out["verdict"], str) and out["verdict"]
    assert {"dimension", "metric", "a_value", "b_value", "winner"} <= set(out["comparisons"][0])


def test_endpoint(client, tmp_path):
    a = _write(tmp_path, "a.wav")
    b = _write(tmp_path, "b.wav", clip=True)
    resp = client.post("/copilot/ab_critique", json={"path_a": a, "path_b": b})
    assert resp.status_code == 200
    assert "verdict" in resp.json() and "overall" in resp.json()


def test_endpoint_404(client, tmp_path):
    a = _write(tmp_path, "a.wav")
    resp = client.post("/copilot/ab_critique", json={"path_a": a, "path_b": "/nope.wav"})
    assert resp.status_code == 404
