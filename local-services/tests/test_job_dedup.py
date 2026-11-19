"""Per-session/operation job deduplication (2026-07-18 audit ARCH-01 remainder).

A double-click or a client retrying a slow request previously admitted two
distinct jobs doing the same work. JobStore.admit_or_join() lets a second
request with the same dedup_key join the already-running job instead.
"""
from khensu_services.common.jobs import JobStore, admit_or_join_or_429
from fastapi import HTTPException
import pytest


def test_admit_or_join_creates_a_new_job_when_no_active_match():
    store = JobStore(max_active=5)
    job_id, created = store.admit_or_join("a", {"status": "pending"}, "key-1")
    assert created is True
    assert job_id == "a"
    assert "a" in store


def test_admit_or_join_joins_an_existing_active_job_with_the_same_key():
    store = JobStore(max_active=5)
    store.admit_or_join("a", {"status": "pending"}, "key-1")

    job_id, created = store.admit_or_join("b", {"status": "pending"}, "key-1")
    assert created is False
    assert job_id == "a"  # joined the first job, "b" was never registered
    assert "b" not in store
    assert len(store) == 1


def test_admit_or_join_does_not_join_a_terminal_job_with_the_same_key():
    store = JobStore(max_active=5)
    store.admit_or_join("a", {"status": "pending"}, "key-1")
    store["a"]["status"] = "completed"

    job_id, created = store.admit_or_join("b", {"status": "pending"}, "key-1")
    assert created is True
    assert job_id == "b"
    assert "b" in store


def test_admit_or_join_treats_different_keys_independently():
    store = JobStore(max_active=5)
    id1, created1 = store.admit_or_join("a", {"status": "pending"}, "key-1")
    id2, created2 = store.admit_or_join("b", {"status": "pending"}, "key-2")
    assert created1 and created2
    assert id1 == "a" and id2 == "b"


def test_admit_or_join_with_no_dedup_key_always_creates():
    store = JobStore(max_active=5)
    id1, created1 = store.admit_or_join("a", {"status": "pending"}, None)
    id2, created2 = store.admit_or_join("b", {"status": "pending"}, "")
    assert created1 and created2
    assert id1 == "a" and id2 == "b"


def test_admit_or_join_or_429_raises_when_at_capacity_and_no_existing_job():
    store = JobStore(max_active=1)
    admit_or_join_or_429(store, "a", {"status": "pending"}, "key-1")
    with pytest.raises(HTTPException) as exc_info:
        admit_or_join_or_429(store, "b", {"status": "pending"}, "key-2")
    assert exc_info.value.status_code == 429


def test_admit_or_join_or_429_joins_instead_of_raising_when_at_capacity(monkeypatch):
    store = JobStore(max_active=1)
    job_id, created = admit_or_join_or_429(store, "a", {"status": "pending"}, "key-1")
    assert created is True

    # Store is now at capacity (1/1 active), but the same key should join
    # rather than being rejected.
    joined_id, joined_created = admit_or_join_or_429(store, "b", {"status": "pending"}, "key-1")
    assert joined_created is False
    assert joined_id == job_id


def test_admit_or_join_frees_the_dedup_slot_once_the_job_completes():
    store = JobStore(max_active=5)
    job_id, _ = store.admit_or_join("a", {"status": "pending"}, "key-1")
    store[job_id]["status"] = "completed"

    new_id, created = store.admit_or_join("b", {"status": "pending"}, "key-1")
    assert created is True
    assert new_id == "b"


def test_double_click_on_apply_chain_start_joins_the_same_job(client, tmp_path):
    """End-to-end: two identical /apply_chain/start requests for the same
    (file, output) — the classic double-click — must return the same
    job_id, not two distinct jobs racing to render the same output."""
    import numpy as np
    import soundfile as sf

    sr = 22050
    y = 0.3 * np.sin(2 * np.pi * 440.0 * np.linspace(0, 1.0, sr, endpoint=False))
    src = tmp_path / "in.wav"
    sf.write(str(src), np.stack([y, y], axis=1), sr)

    body = {
        "file_path": str(src),
        "output_path": str(tmp_path / "out.wav"),
        "effects": [{"effect": "Gain", "parameters": {"gain_db": -3.0}}],
    }
    resp1 = client.post("/copilot/apply_chain/start", json=body)
    resp2 = client.post("/copilot/apply_chain/start", json=body)

    assert resp1.status_code == 200
    assert resp2.status_code == 200
    assert resp1.json()["job_id"] == resp2.json()["job_id"]
