"""Tests for the apply_chain and generate_mixer_chain job-queue endpoints
(start/status/cancel) — addresses the audit finding that render/LLM
operations had no cancellation, unlike /analyze/start and /separate/start."""
from __future__ import annotations

import os
import time

import numpy as np
import pytest
import soundfile as sf
from fastapi.testclient import TestClient

from khensu_services.server import app

client = TestClient(app)


@pytest.fixture
def tone_wav(tmp_path):
    sr = 44100
    t = np.arange(sr) / sr
    y = (0.3 * np.sin(2 * np.pi * 440.0 * t)).astype(np.float32)
    p = tmp_path / "tone.wav"
    sf.write(str(p), y, sr)
    return str(p)


def _poll_until_done(status_url: str, timeout_sec: float = 10.0) -> dict:
    deadline = time.monotonic() + timeout_sec
    while time.monotonic() < deadline:
        resp = client.get(status_url)
        assert resp.status_code == 200
        data = resp.json()
        if data["status"] in ("completed", "failed", "cancelled"):
            return data
        time.sleep(0.05)
    raise AssertionError(f"job at {status_url} did not finish within {timeout_sec}s")


def test_apply_chain_job_completes_and_renders_file(tone_wav, tmp_path):
    out_path = str(tmp_path / "out.wav")
    resp = client.post("/copilot/apply_chain/start", json={
        "file_path": tone_wav,
        "output_path": out_path,
        "effects": [{"effect": "Gain", "parameters": {"gain_db": -6.0}}],
    })
    assert resp.status_code == 200
    job_id = resp.json()["job_id"]

    final = _poll_until_done(f"/copilot/apply_chain/status/{job_id}")
    assert final["status"] == "completed"
    assert "Gain" in final["result"]["applied_effects"]
    assert os.path.isfile(out_path)


def test_apply_chain_job_status_404_for_unknown_job():
    resp = client.get("/copilot/apply_chain/status/not-a-real-job-id")
    assert resp.status_code == 404


def test_apply_chain_job_missing_source_file_returns_404(tmp_path):
    resp = client.post("/copilot/apply_chain/start", json={
        "file_path": "/nonexistent/source.wav",
        "output_path": str(tmp_path / "out.wav"),
        "effects": [],
    })
    assert resp.status_code == 404


def test_apply_chain_job_blocks_unconfirmed_genre_action(tone_wav, tmp_path):
    resp = client.post("/copilot/apply_chain/start", json={
        "file_path": tone_wav,
        "output_path": str(tmp_path / "out.wav"),
        "genre_relative": True,
        "genre_action_allowed": False,
        "abstained_reason": "candidate set is unconfirmed",
        "effects": [],
    })
    assert resp.status_code == 409
    assert "unconfirmed" in resp.json()["detail"]


def test_apply_chain_job_can_be_cancelled_before_it_runs(tone_wav, tmp_path):
    out_path = str(tmp_path / "out.wav")
    resp = client.post("/copilot/apply_chain/start", json={
        "file_path": tone_wav,
        "output_path": out_path,
        "effects": [{"effect": "Gain", "parameters": {"gain_db": -6.0}}],
    })
    job_id = resp.json()["job_id"]

    cancel_resp = client.post(f"/copilot/apply_chain/{job_id}/cancel")
    assert cancel_resp.status_code == 200
    assert cancel_resp.json()["ok"] is True

    # Cancelling an already-finished or already-cancelled job is a no-op,
    # not an error — mirrors /separate/{job_id}/cancel's contract.
    cancel_again = client.post(f"/copilot/apply_chain/{job_id}/cancel")
    assert cancel_again.status_code == 200


def test_apply_chain_cancel_on_unknown_job_is_a_no_op():
    resp = client.post("/copilot/apply_chain/not-a-real-job-id/cancel")
    assert resp.status_code == 200
    # SVC-14: ok only for a known job; the real status is reported.
    assert resp.json() == {"ok": False, "status": "unknown"}


def test_generate_mixer_chain_job_completes_with_heuristic_fallback():
    # No ANTHROPIC_API_KEY in the test environment -> heuristic engine.
    resp = client.post("/copilot/generate_mixer_chain/start", json={
        "target_channel": "vocals", "genre": "Pop", "energy": "high", "mood": "modern",
        "bpm": 120.0, "style_description": "test", "enabled_ecosystems": ["Stock FL Studio"],
    })
    assert resp.status_code == 200
    job_id = resp.json()["job_id"]

    final = _poll_until_done(f"/copilot/generate_mixer_chain/status/{job_id}")
    assert final["status"] == "completed"
    assert len(final["result"]["plugins"]) > 0
    assert final["result"]["decision_hash"] is not None


def test_generate_mixer_chain_job_status_404_for_unknown_job():
    resp = client.get("/copilot/generate_mixer_chain/status/not-a-real-job-id")
    assert resp.status_code == 404


def test_generate_mixer_chain_cancel_on_unknown_job_is_a_no_op():
    resp = client.post("/copilot/generate_mixer_chain/not-a-real-job-id/cancel")
    assert resp.status_code == 200
    # SVC-14: ok only for a known job; the real status is reported.
    assert resp.json() == {"ok": False, "status": "unknown"}


def test_chat_job_completes_with_heuristic_fallback():
    # No ANTHROPIC_API_KEY in the test environment -> heuristic engine.
    resp = client.post("/copilot/chat/start", json={
        "message": "make the vocals louder",
    })
    assert resp.status_code == 200
    job_id = resp.json()["job_id"]

    final = _poll_until_done(f"/copilot/chat/status/{job_id}")
    assert final["status"] == "completed"
    assert final["result"]["response"]


def test_chat_job_status_404_for_unknown_job():
    resp = client.get("/copilot/chat/status/not-a-real-job-id")
    assert resp.status_code == 404


def test_chat_cancel_on_unknown_job_is_a_no_op():
    resp = client.post("/copilot/chat/not-a-real-job-id/cancel")
    assert resp.status_code == 200
    # SVC-14: ok only for a known job; the real status is reported.
    assert resp.json() == {"ok": False, "status": "unknown"}


def test_chat_job_can_be_cancelled_before_it_runs():
    resp = client.post("/copilot/chat/start", json={"message": "test message"})
    job_id = resp.json()["job_id"]

    cancel_resp = client.post(f"/copilot/chat/{job_id}/cancel")
    assert cancel_resp.status_code == 200
    assert cancel_resp.json()["ok"] is True


def test_mastering_agent_job_converges_or_exhausts_budget(tone_wav):
    resp = client.post("/copilot/mastering_agent/start", json={
        "file_path": tone_wav, "genre_id": "trap", "genre_action_allowed": True, "max_iterations": 3,
    })
    assert resp.status_code == 200
    job_id = resp.json()["job_id"]

    final = _poll_until_done(f"/copilot/mastering_agent/status/{job_id}", timeout_sec=30.0)
    assert final["status"] == "completed"
    result = final["result"]
    assert 1 <= len(result["iterations"]) <= 3
    first = result["iterations"][0]
    assert "metrics" in first and "deltas" in first
    assert os.path.isfile(first["output_path"])
    assert result["decision_hash"]
    assert "guardrail" in result


def test_mastering_agent_job_missing_source_file_returns_404(tmp_path):
    resp = client.post("/copilot/mastering_agent/start", json={
        "file_path": "/nonexistent/source.wav", "genre_id": "trap", "genre_action_allowed": True,
    })
    assert resp.status_code == 404


def test_mastering_agent_job_can_be_cancelled_before_it_runs(tone_wav):
    resp = client.post("/copilot/mastering_agent/start", json={
        "file_path": tone_wav, "genre_id": "trap", "genre_action_allowed": True,
    })
    job_id = resp.json()["job_id"]

    cancel_resp = client.post(f"/copilot/mastering_agent/{job_id}/cancel")
    assert cancel_resp.status_code == 200
    assert cancel_resp.json()["ok"] is True


def test_mastering_agent_status_404_for_unknown_job():
    resp = client.get("/copilot/mastering_agent/status/not-a-real-job-id")
    assert resp.status_code == 404


def test_section_chain_job_applies_different_effects_per_section(tone_wav, tmp_path):
    out_path = str(tmp_path / "sectioned.wav")
    resp = client.post("/copilot/apply_section_chain/start", json={
        "file_path": tone_wav,
        "output_path": out_path,
        "sections": [
            {"label": "verse", "start_sec": 0.0, "end_sec": 0.5, "effects": []},
            {"label": "chorus", "start_sec": 0.5, "end_sec": 1.0,
             "effects": [{"effect": "Gain", "parameters": {"gain_db": -6.0}}]},
        ],
    })
    assert resp.status_code == 200
    job_id = resp.json()["job_id"]

    final = _poll_until_done(f"/copilot/apply_section_chain/status/{job_id}")
    assert final["status"] == "completed"
    result = final["result"]
    assert os.path.isfile(out_path)
    labels = [s["label"] for s in result["sections"]]
    assert labels == ["verse", "chorus"]
    assert "Gain" in result["sections"][1]["applied"]
    assert result["sections"][0]["applied"] == []
    assert "guardrail" in result


def test_section_chain_job_requires_at_least_one_section(tone_wav, tmp_path):
    resp = client.post("/copilot/apply_section_chain/start", json={
        "file_path": tone_wav, "output_path": str(tmp_path / "out.wav"), "sections": [],
    })
    assert resp.status_code == 422


def test_section_chain_job_rejects_overlapping_sections(tone_wav, tmp_path):
    resp = client.post("/copilot/apply_section_chain/start", json={
        "file_path": tone_wav,
        "output_path": str(tmp_path / "out.wav"),
        "sections": [
            {"start_sec": 0.0, "end_sec": 0.75, "effects": []},
            {"start_sec": 0.5, "end_sec": 1.0, "effects": []},
        ],
    })
    assert resp.status_code == 422


def test_section_chain_job_missing_source_file_returns_404(tmp_path):
    resp = client.post("/copilot/apply_section_chain/start", json={
        "file_path": "/nonexistent/source.wav",
        "output_path": str(tmp_path / "out.wav"),
        "sections": [{"start_sec": 0.0, "end_sec": 1.0, "effects": []}],
    })
    assert resp.status_code == 404


def test_section_chain_status_404_for_unknown_job():
    resp = client.get("/copilot/apply_section_chain/status/not-a-real-job-id")
    assert resp.status_code == 404


def test_section_chain_cancel_on_unknown_job_is_a_no_op():
    resp = client.post("/copilot/apply_section_chain/not-a-real-job-id/cancel")
    assert resp.status_code == 200
    # SVC-14: ok only for a known job; the real status is reported.
    assert resp.json() == {"ok": False, "status": "unknown"}


def test_apply_chain_events_stream_until_terminal(tone_wav, tmp_path):
    # PERF-6: the status as server-sent events, ending at a terminal state.
    import json
    out_path = str(tmp_path / "out_sse.wav")
    job_id = client.post("/copilot/apply_chain/start", json={
        "file_path": tone_wav, "output_path": out_path,
        "effects": [{"effect": "Gain", "parameters": {"gain_db": -3.0}}],
    }).json()["job_id"]
    events = []
    with client.stream("GET", f"/copilot/apply_chain/events/{job_id}") as resp:
        assert resp.status_code == 200
        assert resp.headers["content-type"].startswith("text/event-stream")
        for line in resp.iter_lines():
            if line.startswith("data: "):
                events.append(json.loads(line[len("data: "):]))
    assert events, "no events"
    assert events[-1]["status"] == "completed"
    assert events[-1] == client.get(f"/copilot/apply_chain/status/{job_id}").json()
    # One event per change, never a repeat.
    assert all(a != b for a, b in zip(events, events[1:]))


def test_apply_chain_events_404_for_unknown_job():
    assert client.get("/copilot/apply_chain/events/not-a-job").status_code == 404
