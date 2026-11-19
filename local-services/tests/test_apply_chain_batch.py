"""Tests for /copilot/apply_chain/batch — folder-wide chain rendering
(generalizes the single-file apply_chain job queue)."""
from __future__ import annotations

import os
import time

import numpy as np
import pytest
import soundfile as sf
from fastapi.testclient import TestClient

from khensu_services.server import app

client = TestClient(app)

SR = 22050


def _write_tone(path, freq=440.0, seconds=0.5):
    t = np.arange(int(SR * seconds)) / SR
    sf.write(str(path), (0.3 * np.sin(2 * np.pi * freq * t)).astype(np.float32), SR)
    return str(path)


@pytest.fixture
def tone_folder(tmp_path):
    folder = tmp_path / "stems"
    folder.mkdir()
    for i, freq in enumerate((220.0, 440.0, 880.0)):
        _write_tone(folder / f"tone{i}.wav", freq)
    # Non-audio clutter must be ignored, not treated as a failure.
    (folder / "notes.txt").write_text("not audio")
    return folder


def _poll_until_done(status_url: str, timeout_sec: float = 15.0) -> dict:
    deadline = time.monotonic() + timeout_sec
    while time.monotonic() < deadline:
        resp = client.get(status_url)
        assert resp.status_code == 200
        data = resp.json()
        if data["status"] in ("completed", "failed", "cancelled"):
            return data
        time.sleep(0.05)
    raise AssertionError(f"job at {status_url} did not finish within {timeout_sec}s")


def test_batch_processes_every_audio_file_in_folder(tone_folder):
    resp = client.post("/copilot/apply_chain/batch/start", json={
        "input_dir": str(tone_folder),
        "effects": [{"effect": "Gain", "parameters": {"gain_db": -6.0}}],
    })
    assert resp.status_code == 200
    body = resp.json()
    assert body["total_files"] == 3
    job_id = body["job_id"]

    final = _poll_until_done(f"/copilot/apply_chain/batch/status/{job_id}")
    assert final["status"] == "completed"
    assert final["progress"] == {"total": 3, "done": 3, "failed": 0}

    results = final["results"]
    assert len(results) == 3
    for r in results:
        assert r["status"] == "success"
        assert "Gain" in r["applied_effects"]
        assert os.path.isfile(r["output_path"])
    # Default output dir sits inside the input folder.
    assert all("khensu_batch" in r["output_path"] for r in results)


def test_batch_accepts_explicit_file_list(tmp_path):
    files = [_write_tone(tmp_path / f"take{i}.wav") for i in range(2)]
    out_dir = tmp_path / "renders"
    resp = client.post("/copilot/apply_chain/batch/start", json={
        "file_paths": files,
        "output_dir": str(out_dir),
        "suffix": "_mastered",
        "effects": [{"effect": "Gain", "parameters": {"gain_db": -3.0}}],
    })
    assert resp.status_code == 200
    job_id = resp.json()["job_id"]

    final = _poll_until_done(f"/copilot/apply_chain/batch/status/{job_id}")
    assert final["status"] == "completed"
    outs = [r["output_path"] for r in final["results"]]
    assert all(os.path.dirname(o) == str(out_dir) for o in outs)
    assert all(os.path.basename(o).endswith("_mastered.wav") for o in outs)


def test_batch_retains_valid_stem_provenance(tmp_path):
    mix = _write_tone(tmp_path / "mix.wav")
    vocals = _write_tone(tmp_path / "vocals.wav", 330.0)
    resp = client.post("/copilot/apply_chain/batch/start", json={
        "file_paths": [mix],
        "stem_paths": {"vocals": vocals},
        "stem_provenance": {"vocals": {"source_id": "session-v1", "method": "user_export"}},
        "effects": [],
    })
    assert resp.status_code == 200
    final = _poll_until_done(f"/copilot/apply_chain/batch/status/{resp.json()['job_id']}")
    assert final["stem_provenance"]["vocals"]["source_id"] == "session-v1"
    assert final["results"][0]["stem_provenance"]["vocals"]["method"] == "user_export"


def test_batch_continues_past_corrupt_file(tmp_path):
    good = _write_tone(tmp_path / "good.wav")
    bad = tmp_path / "bad.wav"
    bad.write_bytes(b"RIFF not really audio")
    resp = client.post("/copilot/apply_chain/batch/start", json={
        "file_paths": [good, str(bad)],
        "effects": [{"effect": "Gain", "parameters": {"gain_db": -3.0}}],
    })
    assert resp.status_code == 200
    job_id = resp.json()["job_id"]

    final = _poll_until_done(f"/copilot/apply_chain/batch/status/{job_id}")
    # The batch as a whole completes; the corrupt file is recorded per-file.
    assert final["status"] == "completed"
    assert final["progress"]["done"] == 2
    assert final["progress"]["failed"] == 1
    statuses = {os.path.basename(r["file_path"]): r["status"] for r in final["results"]}
    assert statuses["good.wav"] == "success"
    assert statuses["bad.wav"] == "failed"


def test_batch_missing_folder_returns_404():
    resp = client.post("/copilot/apply_chain/batch/start", json={
        "input_dir": "/nonexistent/folder",
        "effects": [],
    })
    assert resp.status_code == 404


def test_batch_with_no_inputs_returns_422(tmp_path):
    empty = tmp_path / "empty"
    empty.mkdir()
    resp = client.post("/copilot/apply_chain/batch/start", json={
        "input_dir": str(empty),
        "effects": [],
    })
    assert resp.status_code == 422


def test_batch_can_be_cancelled(tone_folder):
    resp = client.post("/copilot/apply_chain/batch/start", json={
        "input_dir": str(tone_folder),
        "effects": [{"effect": "Gain", "parameters": {"gain_db": -6.0}}],
    })
    job_id = resp.json()["job_id"]

    cancel = client.post(f"/copilot/apply_chain/batch/{job_id}/cancel")
    assert cancel.status_code == 200
    assert cancel.json()["ok"] is True
    # Cancelling twice is a no-op, mirroring the other job queues.
    assert client.post(f"/copilot/apply_chain/batch/{job_id}/cancel").status_code == 200


def test_batch_status_404_for_unknown_job():
    assert client.get("/copilot/apply_chain/batch/status/nope").status_code == 404


def test_batch_cancel_on_unknown_job_is_a_no_op():
    resp = client.post("/copilot/apply_chain/batch/nope/cancel")
    assert resp.status_code == 200
    # SVC-14: ok only for a known job; the real status is reported.
    assert resp.json() == {"ok": False, "status": "unknown"}
