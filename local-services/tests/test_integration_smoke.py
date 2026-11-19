"""End-to-end integration smoke test: drives a real generated WAV file
through the actual FastAPI app (via TestClient — no live subprocess, avoids
port contention) with nothing mocked, covering both the analysis pipeline
and the stem-separation job lifecycle in one round trip.

This complements (does not replace) the existing per-endpoint suites:
- test_analyze_endpoint.py mocks analyze_quick/analyze_advanced to test the
  HTTP/schema layer in isolation; test_quick_analyzer.py/test_advanced_analyzer.py
  exercise the real analyzer functions directly (no HTTP layer).
- test_separate_endpoint.py already drives /separate fully unmocked, but
  waits for full completion (blocking, via the legacy /separate wrapper).
This file is the one place both pipelines are driven together, unmocked,
through the real HTTP request/response cycle a client actually uses.
"""
import os
import time

import numpy as np
import soundfile as sf
import pytest


@pytest.fixture
def tmp_wav(tmp_path):
    """10s, 44.1kHz stereo sine sweep + light noise -- short enough to keep
    the real (unmocked) quick analyzer and HPSS separation fast in CI."""
    sr = 44100
    duration = 10.0
    t = np.linspace(0, duration, int(sr * duration), endpoint=False)
    sweep = 0.3 * np.sin(2 * np.pi * (200.0 + 800.0 * t / duration) * t)
    noise = 0.02 * np.random.default_rng(42).standard_normal(t.shape[0])
    mono = (sweep + noise).astype(np.float32)
    stereo = np.stack([mono, mono * 0.9], axis=1)
    path = tmp_path / "sample_track.wav"
    sf.write(str(path), stereo, sr)
    return str(path)


def test_full_analysis_round_trip(client, tmp_wav):
    resp = client.get("/health")
    assert resp.status_code == 200

    resp = client.post("/analyze", json={"file_path": tmp_wav, "mode": "quick"})
    assert resp.status_code == 200
    data = resp.json()
    assert data["mode"] == "quick"
    assert isinstance(data["bpm"], float)
    assert isinstance(data["key"], str)
    assert isinstance(data["sections"], list)


def test_stem_separation_job_lifecycle(client, tmp_wav, tmp_path):
    output_dir = str(tmp_path / "stems")

    resp = client.post(
        "/separate/start",
        json={"file_path": tmp_wav, "output_dir": output_dir, "method": "hpss"},
    )
    assert resp.status_code == 200
    job_id = resp.json()["job_id"]

    resp = client.get(f"/separate/status/{job_id}")
    assert resp.status_code == 200
    assert resp.json()["status"] in ("pending", "running", "completed")

    # HPSS is fast (no model download); poll briefly for a real completion
    # rather than only checking the immediate post-submit status.
    status = None
    for _ in range(100):
        resp = client.get(f"/separate/status/{job_id}")
        status = resp.json()
        if status["status"] in ("completed", "failed"):
            break
        time.sleep(0.1)

    assert status["status"] == "completed", status.get("error")
    result = status["result"]
    for key in ("vocals_path", "drums_path", "bass_path", "other_path"):
        assert os.path.exists(result[key])
        assert os.path.getsize(result[key]) > 0
