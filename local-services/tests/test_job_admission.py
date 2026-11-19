"""Bounded job admission (2026-07-18 audit ARCH-01).

JobStore's existing TTL/count eviction only ever bounded *finished* jobs — a
burst of start requests could still submit unboundedly to the shared
executor's unbounded queue. These tests cover the admission cap added on top
of that eviction.
"""
from khensu_services.common.jobs import JobStore, admit_or_429
from fastapi import HTTPException
import pytest


def test_admit_allows_up_to_the_cap_and_rejects_beyond_it():
    store = JobStore(max_active=2)
    assert store.admit("a", {"status": "pending"}) is True
    assert store.admit("b", {"status": "running"}) is True
    assert store.admit("c", {"status": "pending"}) is False
    assert "c" not in store
    assert store.active_count() == 2


def test_admit_counts_only_non_terminal_jobs():
    store = JobStore(max_active=1)
    assert store.admit("a", {"status": "completed"}) is True
    # "a" is already terminal, so it doesn't occupy the one active slot.
    assert store.admit("b", {"status": "pending"}) is True
    assert store.active_count() == 1


def test_admit_frees_a_slot_once_a_job_completes():
    store = JobStore(max_active=1)
    assert store.admit("a", {"status": "pending"}) is True
    assert store.admit("b", {"status": "pending"}) is False
    store["a"]["status"] = "completed"
    assert store.admit("b", {"status": "pending"}) is True


def test_admit_with_no_cap_is_unbounded():
    store = JobStore()  # default max_active=None preserves old behavior
    for i in range(50):
        assert store.admit(str(i), {"status": "pending"}) is True


def test_admit_or_429_raises_http_exception_when_at_capacity():
    store = JobStore(max_active=1)
    admit_or_429(store, "a", {"status": "pending"})
    with pytest.raises(HTTPException) as exc_info:
        admit_or_429(store, "b", {"status": "pending"})
    assert exc_info.value.status_code == 429
    assert "b" not in store


def test_apply_chain_start_returns_429_when_admission_cap_is_exhausted(client, tmp_path, monkeypatch):
    import numpy as np
    import soundfile as sf
    import khensu_services.copilot.router_render as router_render

    sr = 22050
    y = 0.3 * np.sin(2 * np.pi * 440.0 * np.linspace(0, 1.0, sr, endpoint=False))
    src = tmp_path / "in.wav"
    sf.write(str(src), np.stack([y, y], axis=1), sr)

    # Fill the store to capacity with fake pending jobs before hitting the
    # real endpoint, so no actual render work needs to run for this test.
    monkeypatch.setattr(router_render._apply_jobs, "_max_active", 0)

    resp = client.post("/copilot/apply_chain/start", json={
        "file_path": str(src),
        "output_path": str(tmp_path / "out.wav"),
        "effects": [{"effect": "Gain", "parameters": {"gain_db": -3.0}}],
    })
    assert resp.status_code == 429
