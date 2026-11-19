"""JobStore admission atomicity + cancel handshake (audit M4)."""
from __future__ import annotations

import threading

from khensu_services.common.jobs import (
    JobStore, cancel_job, job_begin, job_cancel_requested, job_settle, job_update,
)


def test_concurrent_admit_never_exceeds_cap():
    store = JobStore(name="t", max_active=3)
    barrier = threading.Barrier(16)
    admitted = []

    def worker(i):
        barrier.wait()
        if store.admit(f"j{i}", {"status": "pending"}):
            admitted.append(i)

    threads = [threading.Thread(target=worker, args=(i,)) for i in range(16)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    assert len(admitted) == 3
    assert store.active_count() == 3


def test_concurrent_admit_or_join_dedups():
    store = JobStore(name="t2", max_active=10)
    barrier = threading.Barrier(12)
    results = []

    def worker(i):
        barrier.wait()
        results.append(store.admit_or_join(f"j{i}", {"status": "pending"}, "same-key"))

    threads = [threading.Thread(target=worker, args=(i,)) for i in range(12)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    assert sum(1 for _jid, created in results if created) == 1
    assert len({jid for jid, _ in results}) == 1


def test_cancel_without_futures_is_cancelling_until_worker_acks():
    store = JobStore(name="t3", max_active=1, max_finished=0)
    store["a"] = {"status": "pending"}
    assert job_begin(store, "a")
    assert cancel_job(store, "a") == "cancelling"
    # Still counts toward the cap and survives pruning (not terminal).
    assert store.active_count() == 1
    assert not store.admit("b", {"status": "pending"})
    store.prune()
    assert "a" in store
    assert job_cancel_requested(store, "a")
    job_update(store, "a", stage="x")
    assert "stage" not in store["a"]
    # The worker's final write acknowledges the cancel instead of completing.
    assert job_settle(store, "a", status="completed", result=1) is False
    assert store.get("a", {}).get("status") in ("cancelled", None)
    assert store.admit("b", {"status": "pending"})


def test_cancel_before_start_is_acknowledged_by_begin():
    store = JobStore(name="t4")
    store["a"] = {"status": "pending"}
    assert cancel_job(store, "a") == "cancelling"
    assert job_begin(store, "a") is False
    assert store["a"]["status"] == "cancelled"


def test_worker_helpers_tolerate_evicted_job():
    store = JobStore(name="t5")
    assert job_begin(store, "gone") is False
    assert job_settle(store, "gone", status="completed") is False
    assert job_cancel_requested(store, "gone") is True
    job_update(store, "gone", stage="x")   # no KeyError
