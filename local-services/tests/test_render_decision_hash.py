"""decision_hash on the render routes.

A chat/mixer-chain/mastering-agent response carries ``decision_hash``
(llm_config.decision_hash). When the desktop renders that proposal it forwards
the hash, and the render's provenance event must record it so the render can be
traced back to exactly which model decision produced it. A render with no hash
must not grow the key, and a malformed hash is rejected rather than copied into
the provenance store.
"""
from __future__ import annotations

import os
import time

import numpy as np
import pytest
import soundfile as sf

_HASH = "0123456789abcdef"
_GAIN = [{"effect": "Gain", "parameters": {"gain_db": -3.0}}]


@pytest.fixture
def isolated_events_db(tmp_path, monkeypatch):
    import khensu_services.events.db as edb
    monkeypatch.setenv("KHENSU_EVENTS_DB", str(tmp_path / "ev.db"))
    old = edb._conn
    edb._conn = None
    yield
    if edb._conn is not None:
        edb._conn.close()
    edb._conn = old


@pytest.fixture
def mono_wav(tmp_path):
    sr = 22050
    t = np.linspace(0, 1, sr, endpoint=False)
    y = (0.3 * np.sin(2 * np.pi * 220 * t)).astype(np.float32)
    p = tmp_path / "src.wav"
    sf.write(str(p), y, sr)
    return str(p)


def _poll(client, url: str, timeout_sec: float = 20.0) -> dict:
    deadline = time.monotonic() + timeout_sec
    while time.monotonic() < deadline:
        data = client.get(url).json()
        if data["status"] in ("completed", "failed", "cancelled", "interrupted"):
            return data
        time.sleep(0.05)
    raise AssertionError(f"{url} did not finish within {timeout_sec}s")


def _events(client, session_id: str) -> list[dict]:
    r = client.get("/events", params={"session_id": session_id})
    assert r.status_code == 200, r.text
    return r.json()["events"]


def test_sync_apply_chain_records_decision_hash(client, isolated_events_db, mono_wav, tmp_path):
    r = client.post("/copilot/apply_chain", json={
        "file_path": mono_wav, "output_path": str(tmp_path / "out.wav"), "effects": _GAIN,
        "session_id": "s-sync", "decision_hash": _HASH,
    })
    assert r.status_code == 200, r.text
    (ev,) = _events(client, "s-sync")
    assert ev["event_type"] == "chain_applied"
    assert ev["payload"]["decision_hash"] == _HASH


def test_apply_chain_without_decision_hash_omits_it(client, isolated_events_db, mono_wav, tmp_path):
    r = client.post("/copilot/apply_chain", json={
        "file_path": mono_wav, "output_path": str(tmp_path / "out.wav"), "effects": _GAIN,
        "session_id": "s-none",
    })
    assert r.status_code == 200, r.text
    (ev,) = _events(client, "s-none")
    assert "decision_hash" not in ev["payload"]


def test_apply_chain_job_records_decision_hash_in_event_and_result(client, isolated_events_db, mono_wav, tmp_path):
    r = client.post("/copilot/apply_chain/start", json={
        "file_path": mono_wav, "output_path": str(tmp_path / "out.wav"), "effects": _GAIN,
        "session_id": "s-job", "decision_hash": _HASH,
    })
    assert r.status_code == 200, r.text
    final = _poll(client, f"/copilot/apply_chain/status/{r.json()['job_id']}")
    assert final["status"] == "completed", final
    assert final["result"]["decision_hash"] == _HASH
    (ev,) = _events(client, "s-job")
    assert ev["payload"]["decision_hash"] == _HASH


def test_apply_chain_job_without_hash_has_no_result_key(client, isolated_events_db, mono_wav, tmp_path):
    r = client.post("/copilot/apply_chain/start", json={
        "file_path": mono_wav, "output_path": str(tmp_path / "plain.wav"), "effects": _GAIN,
    })
    assert r.status_code == 200, r.text
    final = _poll(client, f"/copilot/apply_chain/status/{r.json()['job_id']}")
    assert final["status"] == "completed", final
    assert "decision_hash" not in final["result"]


def test_batch_apply_chain_records_decision_hash(client, isolated_events_db, mono_wav, tmp_path):
    r = client.post("/copilot/apply_chain/batch/start", json={
        "file_paths": [mono_wav], "output_dir": str(tmp_path / "batch"), "effects": _GAIN,
        "session_id": "s-batch", "decision_hash": _HASH,
    })
    assert r.status_code == 200, r.text
    final = _poll(client, f"/copilot/apply_chain/batch/status/{r.json()['job_id']}")
    assert final["status"] == "completed", final
    assert final["decision_hash"] == _HASH
    (ev,) = _events(client, "s-batch")
    assert ev["payload"]["decision_hash"] == _HASH


def test_section_chain_records_decision_hash(client, isolated_events_db, mono_wav, tmp_path):
    r = client.post("/copilot/apply_section_chain/start", json={
        "file_path": mono_wav, "output_path": str(tmp_path / "sections.wav"),
        "sections": [
            {"label": "verse", "start_sec": 0.0, "end_sec": 0.5, "effects": []},
            {"label": "chorus", "start_sec": 0.5, "end_sec": 1.0, "effects": _GAIN},
        ],
        "session_id": "s-section", "decision_hash": _HASH,
    })
    assert r.status_code == 200, r.text
    final = _poll(client, f"/copilot/apply_section_chain/status/{r.json()['job_id']}")
    assert final["status"] == "completed", final
    assert final["result"]["decision_hash"] == _HASH
    (ev,) = _events(client, "s-section")
    assert ev["event_type"] == "section_chain_applied"
    assert ev["payload"]["decision_hash"] == _HASH


def test_apply_graph_records_decision_hash(client, isolated_events_db, mono_wav, tmp_path):
    r = client.post("/copilot/apply_chain", json={
        "file_path": mono_wav, "output_path": str(tmp_path / "graph.wav"), "effects": [],
        "graph": {"branches": [{"effects": _GAIN}]}, "session_id": "s-graph", "decision_hash": _HASH,
    })
    assert r.status_code == 200, r.text
    (ev,) = _events(client, "s-graph")
    assert ev["payload"]["decision_hash"] == _HASH


def test_override_guardrail_records_decision_hash(client, isolated_events_db, mono_wav, tmp_path, monkeypatch):
    import khensu_services.copilot.render_guardrails as guardrails
    out_path = str(tmp_path / "out.wav")
    # A scoped patch: monkeypatch.undo() would also drop the fixture's
    # KHENSU_EVENTS_DB override and send the event to the real khensu.db.
    with monkeypatch.context() as m:
        m.setattr(guardrails, "post_render_guardrail", lambda *_: {"ok": False, "warnings": ["regressed"]})
        resp = client.post("/copilot/apply_chain", json={
            "file_path": mono_wav, "output_path": out_path, "effects": _GAIN, "decision_hash": _HASH,
        })
    assert resp.status_code == 409
    quarantine_path = resp.json()["detail"]["quarantine_path"]

    r = client.post("/copilot/override_guardrail", json={
        "source_path": mono_wav, "quarantine_path": quarantine_path, "output_path": out_path,
        "session_id": "s-override", "expert_note": "reviewed", "decision_hash": _HASH,
    })
    assert r.status_code == 200, r.text
    assert os.path.isfile(out_path)
    (ev,) = _events(client, "s-override")
    assert ev["event_type"] == "guardrail_override"
    assert ev["payload"]["decision_hash"] == _HASH


@pytest.mark.parametrize("bad", ["not-hex!", "g" * 16, "a" * 65, "abc def"])
@pytest.mark.parametrize("route", [
    "/copilot/apply_chain", "/copilot/apply_chain/start", "/copilot/apply_chain/batch/start",
    "/copilot/apply_section_chain/start", "/copilot/override_guardrail",
])
def test_malformed_decision_hash_is_rejected(client, tmp_path, route, bad):
    # Pydantic validates the body before any file check, so placeholder paths do.
    body = {
        "file_path": str(tmp_path / "x.wav"), "output_path": str(tmp_path / "y.wav"),
        "file_paths": [str(tmp_path / "x.wav")],
        "sections": [{"start_sec": 0.0, "end_sec": 1.0}],
        "source_path": str(tmp_path / "x.wav"), "quarantine_path": str(tmp_path / ".quarantine_y.wav"),
        "expert_note": "n", "decision_hash": bad,
    }
    r = client.post(route, json=body)
    assert r.status_code == 422, r.text
    assert "decision_hash" in r.text


def test_mastering_candidate_event_payload_object_is_accepted(client, isolated_events_db):
    # The desktop's mastering_candidate_accepted event (MasteringAgentPanel)
    # now sends {output_path, decision_hash}: /events requires a dict payload,
    # and the bare-path string it used to send was a 422 the outbox dropped.
    ok = client.post("/events", json={
        "session_id": "s-master", "event_type": "mastering_candidate_accepted", "summary": "accepted",
        "payload": {"output_path": "Z:/mastered.wav", "decision_hash": _HASH},
    })
    assert ok.status_code == 200, ok.text
    assert client.post("/events", json={
        "session_id": "s-master", "event_type": "mastering_candidate_accepted", "payload": "Z:/mastered.wav",
    }).status_code == 422
    (ev,) = _events(client, "s-master")
    assert ev["payload"]["decision_hash"] == _HASH
