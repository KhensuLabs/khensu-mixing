"""Tests for the provenance event layer (/events) and its auto-logging."""
from __future__ import annotations

import numpy as np
import pytest
import soundfile as sf


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


def test_post_and_list_events(client, isolated_events_db):
    r = client.post("/events", json={
        "session_id": "s1", "event_type": "recommendation_applied",
        "summary": "Lowered master 2 dB", "recommendation_id": "lufs_too_loud_f1",
        "payload": {"reduce_gain_db": 2.0},
    })
    assert r.status_code == 200
    assert r.json()["event_type"] == "recommendation_applied"

    lst = client.get("/events", params={"session_id": "s1"})
    assert lst.status_code == 200
    events = lst.json()["events"]
    assert len(events) == 1
    assert events[0]["payload"]["reduce_gain_db"] == 2.0


def test_events_scoped_by_session(client, isolated_events_db):
    client.post("/events", json={"session_id": "a", "event_type": "feedback", "summary": "x"})
    client.post("/events", json={"session_id": "b", "event_type": "feedback", "summary": "y"})
    assert len(client.get("/events", params={"session_id": "a"}).json()["events"]) == 1
    assert len(client.get("/events", params={"session_id": "b"}).json()["events"]) == 1


def test_apply_chain_auto_logs_event(client, isolated_events_db, mono_wav, tmp_path):
    out = str(tmp_path / "applied.wav")
    r = client.post("/copilot/apply_chain", json={
        "file_path": mono_wav, "output_path": out,
        "effects": [{"effect": "Gain", "parameters": {"gain_db": -3.0}}],
        "session_id": "sess42",
        "source_recommendation_id": "over_compressed_x",
    })
    assert r.status_code == 200, r.text

    applied = client.get("/events", params={"session_id": "sess42"})
    assert applied.status_code == 200
    events = applied.json()["events"]
    assert len(events) == 1
    assert events[0]["event_type"] == "chain_applied"
    assert events[0]["recommendation_id"] == "over_compressed_x"
    assert events[0]["payload"]["output_path"] == out


def test_apply_chain_without_session_logs_nothing(client, isolated_events_db, mono_wav, tmp_path):
    out = str(tmp_path / "nolog.wav")
    r = client.post("/copilot/apply_chain", json={
        "file_path": mono_wav, "output_path": out,
        "effects": [{"effect": "Gain", "parameters": {"gain_db": -1.0}}],
    })
    assert r.status_code == 200
    # No session_id -> no provenance row anywhere.
    assert client.get("/events").json()["events"] == []


# ---------------------------------------------------------------------------
# Audit DATA-02: positively bounded query limits/request sizes, and the
# session/full erasure endpoints.
# ---------------------------------------------------------------------------

def test_negative_limit_is_rejected_instead_of_returning_everything(client, isolated_events_db):
    # Previously `limit=-1` (SQLite's "no limit" sentinel) would have reached
    # the query unbounded - now it 422s at the FastAPI layer.
    for _ in range(5):
        client.post("/events", json={"session_id": "s", "event_type": "feedback", "summary": "x"})
    resp = client.get("/events", params={"limit": -1})
    assert resp.status_code == 422


def test_limit_above_the_cap_is_rejected(client, isolated_events_db):
    resp = client.get("/events", params={"limit": 100_000})
    assert resp.status_code == 422


def test_list_events_clamps_a_negative_limit_defensively_even_called_directly(isolated_events_db):
    # Defense in depth: list_events() itself must never pass a negative
    # limit through to SQLite, regardless of caller (not just the router).
    import khensu_services.events.db as edb
    for i in range(3):
        edb.append_event(session_id="s", event_type="feedback", summary=str(i))
    assert len(edb.list_events(session_id="s", limit=-1)) <= 1000


def test_oversized_payload_is_rejected(client, isolated_events_db):
    resp = client.post("/events", json={
        "session_id": "s", "event_type": "feedback", "summary": "x",
        "payload": {"blob": "x" * 40_000},
    })
    assert resp.status_code == 422


def test_delete_events_for_session_only_removes_that_sessions_events(client, isolated_events_db):
    client.post("/events", json={"session_id": "a", "event_type": "feedback", "summary": "x"})
    client.post("/events", json={"session_id": "b", "event_type": "feedback", "summary": "y"})

    resp = client.delete("/events/session/a")
    assert resp.status_code == 200
    assert resp.json()["deleted"] == 1

    assert client.get("/events", params={"session_id": "a"}).json()["events"] == []
    assert len(client.get("/events", params={"session_id": "b"}).json()["events"]) == 1


def test_delete_all_events_removes_everything(client, isolated_events_db):
    client.post("/events", json={"session_id": "a", "event_type": "feedback", "summary": "x"})
    client.post("/events", json={"session_id": "b", "event_type": "feedback", "summary": "y"})

    resp = client.delete("/events")
    assert resp.status_code == 200
    assert resp.json()["deleted"] == 2
    assert client.get("/events").json()["events"] == []

def test_overlong_summary_is_clipped_not_rejected(client, isolated_events_db):
    # AUDIT-056: the desktop outbox dead-letters 4xx responses, so a long
    # summary must not turn into a permanently lost event.
    resp = client.post("/events", json={"session_id": "s", "event_type": "feedback", "summary": "y" * 5000})
    assert resp.status_code == 200
    assert len(resp.json()["summary"]) == 2000
    assert resp.json()["summary"].endswith("...")


def test_malformed_identifier_is_still_rejected(client, isolated_events_db):
    resp = client.post("/events", json={"session_id": "s" * 500, "event_type": "feedback", "summary": "x"})
    assert resp.status_code == 422
