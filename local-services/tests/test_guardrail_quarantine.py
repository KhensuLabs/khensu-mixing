"""Guardrail quarantine/promotion and the explicit expert override endpoint
(audit AI-01 remainder).

A render is written to a QUARANTINE path (`.quarantine_<basename>`), never
directly to the caller's requested output_path - only a passing post-render
guardrail (or an explicit /copilot/override_guardrail call) atomically
promotes it there. This closes the gap where a rejected/missing/errored
guardrail still left a file sitting at the exact path the desktop/user asked
for, indistinguishable on disk from a genuinely applied render.
"""
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


def _quarantine_path(output_path: str) -> str:
    d = os.path.dirname(output_path)
    return os.path.join(d, ".quarantine_" + os.path.basename(output_path))


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


def test_apply_chain_sync_success_leaves_no_stray_quarantine_file(tone_wav, tmp_path):
    out_path = str(tmp_path / "out.wav")
    resp = client.post("/copilot/apply_chain", json={
        "file_path": tone_wav, "output_path": out_path,
        "effects": [{"effect": "Gain", "parameters": {"gain_db": -3.0}}],
    })
    assert resp.status_code == 200
    assert os.path.isfile(out_path)
    assert not os.path.isfile(_quarantine_path(out_path))


def test_apply_chain_start_job_rejection_quarantines_instead_of_promoting(tone_wav, tmp_path, monkeypatch):
    import khensu_services.copilot.render_guardrails as guardrails
    monkeypatch.setattr(guardrails, "post_render_guardrail",
                        lambda *_: {"ok": False, "warnings": ["true peak regressed"]})

    out_path = str(tmp_path / "out.wav")
    resp = client.post("/copilot/apply_chain/start", json={
        "file_path": tone_wav, "output_path": out_path,
        "effects": [{"effect": "Gain", "parameters": {"gain_db": 1.0}}],
    })
    assert resp.status_code == 200
    job_id = resp.json()["job_id"]
    final = _poll_until_done(f"/copilot/apply_chain/status/{job_id}")

    assert final["status"] == "failed"
    assert final["result"]["status"] == "rejected"
    assert final["result"]["guardrail_status"] == "rejected"
    assert not os.path.isfile(out_path)
    assert os.path.isfile(final["result"]["quarantine_path"])


def test_apply_graph_rejection_quarantines_instead_of_promoting(tone_wav, tmp_path, monkeypatch):
    import khensu_services.copilot.render_guardrails as guardrails
    monkeypatch.setattr(guardrails, "post_render_guardrail", lambda *_: {"ok": False, "warnings": ["regressed"]})

    out_path = str(tmp_path / "out.wav")
    resp = client.post("/copilot/apply_chain", json={
        "file_path": tone_wav, "output_path": out_path, "effects": [],
        "graph": {"branches": [{"effects": [{"effect": "Gain", "parameters": {"gain_db": 1.0}}]}]},
    })
    assert resp.status_code == 409
    detail = resp.json()["detail"]
    assert detail["kind"] == "rejected"
    assert not os.path.isfile(out_path)
    assert os.path.isfile(detail["quarantine_path"])


def test_section_chain_job_rejection_quarantines_instead_of_promoting(tmp_path, monkeypatch):
    import khensu_services.copilot.render_guardrails as guardrails
    monkeypatch.setattr(guardrails, "post_render_guardrail", lambda *_: {"ok": False, "warnings": ["regressed"]})

    sr = 44100
    t = np.arange(2 * sr) / sr
    y = (0.3 * np.sin(2 * np.pi * 440.0 * t)).astype(np.float32)
    src = str(tmp_path / "src.wav")
    sf.write(src, y, sr)
    out_path = str(tmp_path / "out.wav")

    resp = client.post("/copilot/apply_section_chain/start", json={
        "file_path": src, "output_path": out_path,
        "sections": [{"label": "a", "start_sec": 0.0, "end_sec": 2.0, "effects": []}],
    })
    assert resp.status_code == 200
    job_id = resp.json()["job_id"]
    final = _poll_until_done(f"/copilot/apply_section_chain/status/{job_id}")

    assert final["status"] == "failed"
    assert final["result"]["guardrail_status"] == "rejected"
    assert not os.path.isfile(out_path)
    assert os.path.isfile(final["result"]["quarantine_path"])


def test_override_guardrail_promotes_a_quarantined_render(tone_wav, tmp_path, monkeypatch):
    import khensu_services.copilot.render_guardrails as guardrails
    out_path = str(tmp_path / "out.wav")
    # Scoped patch, not monkeypatch.undo(): undo() also reverts every other
    # patch in the test (incl. any fixture's KHENSU_EVENTS_DB redirect), which
    # sent another test's events into the real khensu.db.
    with monkeypatch.context() as m:
        m.setattr(guardrails, "post_render_guardrail", lambda *_: {"ok": False, "warnings": ["regressed"]})
        resp = client.post("/copilot/apply_chain", json={
            "file_path": tone_wav, "output_path": out_path,
            "effects": [{"effect": "Gain", "parameters": {"gain_db": 1.0}}],
        })
    assert resp.status_code == 409
    quarantine_path = resp.json()["detail"]["quarantine_path"]
    assert os.path.isfile(quarantine_path)
    assert not os.path.isfile(out_path)

    # Recovery: the real guardrail (unpatched) would now pass against the
    # already-quarantined file, but the override path must work regardless -
    # an expert explicitly accepts responsibility for promoting it.
    override_resp = client.post("/copilot/override_guardrail", json={
        "source_path": tone_wav,
        "quarantine_path": quarantine_path,
        "output_path": out_path,
        "expert_note": "Reviewed the measurements manually - the peak delta is a false positive from silence padding.",
    })
    assert override_resp.status_code == 200, override_resp.text
    body = override_resp.json()
    assert body["status"] == "success"
    assert body["output_path"] == out_path
    assert "measurements" in body
    assert os.path.isfile(out_path)
    assert not os.path.isfile(quarantine_path)


def test_override_guardrail_requires_a_non_empty_expert_note(tone_wav, tmp_path):
    resp = client.post("/copilot/override_guardrail", json={
        "source_path": tone_wav,
        "quarantine_path": str(tmp_path / ".quarantine_out.wav"),
        "output_path": str(tmp_path / "out.wav"),
        "expert_note": "",
    })
    assert resp.status_code == 422  # pydantic min_length violation


def test_override_guardrail_404s_when_quarantine_file_is_missing(tone_wav, tmp_path):
    resp = client.post("/copilot/override_guardrail", json={
        "source_path": tone_wav,
        "quarantine_path": str(tmp_path / ".quarantine_nonexistent.wav"),
        "output_path": str(tmp_path / "nonexistent.wav"),
        "expert_note": "Approved anyway.",
    })
    assert resp.status_code == 404


def test_successful_render_reports_validated_state(tone_wav, tmp_path):
    out_path = str(tmp_path / "out.wav")
    resp = client.post("/copilot/apply_chain", json={
        "file_path": tone_wav, "output_path": out_path,
        "effects": [{"effect": "Gain", "parameters": {"gain_db": -3.0}}],
    })
    assert resp.status_code == 200
    assert resp.json()["render_state"] == "validated"


def test_rejected_render_reports_rejected_state(tone_wav, tmp_path, monkeypatch):
    import khensu_services.copilot.render_guardrails as guardrails
    monkeypatch.setattr(guardrails, "post_render_guardrail", lambda *_: {"ok": False, "warnings": ["regressed"]})
    resp = client.post("/copilot/apply_chain", json={
        "file_path": tone_wav, "output_path": str(tmp_path / "out.wav"),
        "effects": [{"effect": "Gain", "parameters": {"gain_db": 1.0}}],
    })
    assert resp.status_code == 409
    assert resp.json()["detail"]["render_state"] == "rejected"


def test_errored_guardrail_reports_unvalidated_state(tone_wav, tmp_path, monkeypatch):
    import khensu_services.copilot.render_guardrails as guardrails

    def boom(*_args):
        raise RuntimeError("probe blew up")
    monkeypatch.setattr(guardrails, "post_render_guardrail", boom)
    resp = client.post("/copilot/apply_chain", json={
        "file_path": tone_wav, "output_path": str(tmp_path / "out.wav"),
        "effects": [{"effect": "Gain", "parameters": {"gain_db": 1.0}}],
    })
    assert resp.status_code == 409
    # The check couldn't run -> safety unknown -> "unvalidated", distinct from
    # a render that ran and failed the check ("rejected").
    assert resp.json()["detail"]["render_state"] == "unvalidated"


def test_a_hanging_guardrail_fails_closed_via_timeout(tone_wav, tmp_path, monkeypatch):
    import time as _time
    import khensu_services.copilot.render_guardrails as guardrails
    import khensu_services.copilot.router_render as router_render

    # Shrink the deadline so the test is fast, and make the guardrail hang.
    monkeypatch.setattr(router_render, "_GUARDRAIL_TIMEOUT_SEC", 0.3)

    def _hang(*_args):
        _time.sleep(30)  # far longer than the deadline
        return {"ok": True}
    monkeypatch.setattr(guardrails, "post_render_guardrail", _hang)

    output_path = str(tmp_path / "out.wav")
    resp = client.post("/copilot/apply_chain", json={
        "file_path": tone_wav, "output_path": output_path,
        "effects": [{"effect": "Gain", "parameters": {"gain_db": 1.0}}],
    })
    # A hang must fail closed (409), quarantine (nothing at output_path), and
    # report "unvalidated" (the check couldn't run) - not block forever.
    assert resp.status_code == 409
    detail = resp.json()["detail"]
    assert "timed out" in detail["message"].lower()
    assert detail["render_state"] == "unvalidated"
    import os
    assert not os.path.isfile(output_path)


def test_render_state_for_kind_mapping():
    from khensu_services.copilot.router import (
        render_state_for_kind, RENDER_STATE_REJECTED, RENDER_STATE_UNVALIDATED,
    )
    assert render_state_for_kind("rejected") == RENDER_STATE_REJECTED
    assert render_state_for_kind("missing") == RENDER_STATE_UNVALIDATED
    assert render_state_for_kind("error") == RENDER_STATE_UNVALIDATED


def test_override_guardrail_422s_when_quarantine_path_does_not_match_output_path(tone_wav, tmp_path):
    quarantine_path = str(tmp_path / ".quarantine_other.wav")
    sf.write(quarantine_path, np.zeros(100, dtype=np.float32), 44100)
    resp = client.post("/copilot/override_guardrail", json={
        "source_path": tone_wav,
        "quarantine_path": quarantine_path,
        "output_path": str(tmp_path / "out.wav"),  # doesn't match .quarantine_other.wav's convention
        "expert_note": "Approved anyway.",
    })
    assert resp.status_code == 422
