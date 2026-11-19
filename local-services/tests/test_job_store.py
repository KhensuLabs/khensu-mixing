# local-services/tests/test_job_store.py
"""Unit tests for the shared bounded job store + LRU cache (audit F2)."""
from __future__ import annotations

from khensu_services.common.jobs import JobStore
from khensu_services.common.lru import BoundedCache


def test_jobstore_behaves_like_a_dict():
    store = JobStore()
    store["a"] = {"status": "pending"}
    assert "a" in store
    assert store["a"]["status"] == "pending"
    store["a"].update(status="running")
    assert store["a"]["status"] == "running"
    assert store.get("missing") is None


def test_jobstore_keeps_running_jobs_and_recent_finished():
    store = JobStore(retention_sec=10_000, max_finished=20)
    store["r"] = {"status": "running"}
    store["d"] = {"status": "completed"}
    # A new insertion triggers a prune; running + recently-finished both survive.
    store["r2"] = {"status": "running"}
    assert set(store) == {"r", "d", "r2"}


def test_jobstore_evicts_by_ttl_on_next_insert():
    store = JobStore(retention_sec=-1, max_finished=100)  # everything is "too old"
    store["done"] = {"status": "completed"}
    assert "done" in store  # not pruned until the next insert stamps+evicts it
    store["trigger"] = {"status": "running"}
    assert "done" not in store          # evicted (finished + past TTL)
    assert "trigger" in store           # running job retained


def test_jobstore_caps_finished_jobs_oldest_first():
    store = JobStore(retention_sec=10_000, max_finished=2)
    for i in range(4):
        store[f"j{i}"] = {"status": "completed"}
    # A just-inserted job isn't pruned during its own insertion, so the cap is
    # enforced on the *next* insert. Trigger one and verify the finished count
    # settles at max_finished, oldest evicted first.
    store["trigger"] = {"status": "running"}
    finished = [jid for jid, p in store.items() if p["status"] == "completed"]
    assert len(finished) == 2
    assert "j3" in store          # newest finished survives
    assert "j0" not in store      # oldest evicted
    assert "trigger" in store     # running jobs are never capped


def test_jobstore_respects_custom_status_key():
    store = JobStore(status_key="state", retention_sec=-1, max_finished=100)
    store["x"] = {"state": "completed"}
    store["y"] = {"state": "running"}  # triggers prune of the finished x
    assert "x" not in store
    assert "y" in store


def test_jobstore_prune_does_not_leak_bookkeeping_into_payload():
    store = JobStore()
    payload = {"status": "completed", "result": {"ok": True}}
    store["a"] = payload
    store.prune()
    # The job payload must stay exactly what callers spread into responses.
    assert store["a"] == {"status": "completed", "result": {"ok": True}}


def test_bounded_cache_evicts_least_recently_used():
    cache: BoundedCache[str, int] = BoundedCache(max_entries=2)
    cache["a"] = 1
    cache["b"] = 2
    assert cache.get("a") == 1      # touch "a" so "b" is now the LRU entry
    cache["c"] = 3                  # evicts "b"
    assert "b" not in cache
    assert cache.get("a") == 1
    assert cache.get("c") == 3


def test_bounded_cache_pop_and_overwrite():
    cache: BoundedCache[str, int] = BoundedCache(max_entries=2)
    cache["a"] = 1
    cache["a"] = 10                 # overwrite, not a second entry
    assert len(cache) == 1
    assert cache.get("a") == 10
    assert cache.pop("a", None) == 10
    assert cache.pop("missing", None) is None
