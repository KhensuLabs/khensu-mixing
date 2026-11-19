"""common/memory_gate: admission by memory for pool jobs (2026-11-18)."""
from __future__ import annotations

import threading
import time

from khensu_services.common import memory_gate as mg

MB = 2**20


def _gate(budget_mb: float) -> mg.MemoryGate:
    return mg.MemoryGate(budget_bytes=int(budget_mb * MB), enabled=True)


def test_outside_a_pool_job_nothing_is_charged():
    g = _gate(1)
    g.charge("a", 100 * MB)            # the calling thread is not in a job
    assert g.stats()["in_use_bytes"] == 0


def test_a_job_is_charged_once_per_file_and_released_at_its_end():
    g = _gate(10_000)
    g.begin_job()
    g.charge("a", 10 * MB)
    g.charge("a", 10 * MB)             # the same audio again: no second charge
    assert g.stats()["in_use_bytes"] == int(10 * MB * mg.WEIGHT_PER_DECODED_BYTE)
    g.charge("a", 20 * MB)             # a larger load of it: only the difference
    assert g.stats()["in_use_bytes"] == int(20 * MB * mg.WEIGHT_PER_DECODED_BYTE)
    g.end_job()
    assert g.stats()["in_use_bytes"] == 0


def test_a_second_job_waits_until_the_first_releases():
    g = _gate(150)                     # room for one 100 MB charge (10 MB decoded)
    order: list[str] = []
    first_in = threading.Event()
    release_first = threading.Event()

    def first():
        g.begin_job()
        g.charge("a", 10 * MB)
        order.append("first in")
        first_in.set()
        release_first.wait(5)
        order.append("first out")
        g.end_job()

    def second():
        first_in.wait(5)
        g.begin_job()
        g.charge("b", 10 * MB)
        order.append("second in")
        g.end_job()

    t1, t2 = threading.Thread(target=first), threading.Thread(target=second)
    t1.start()
    t2.start()
    time.sleep(0.3)
    assert order == ["first in"]       # the second is waiting
    release_first.set()
    t1.join(5)
    t2.join(5)
    assert order == ["first in", "first out", "second in"]
    assert g.stats()["waits"] == 1 and g.stats()["in_use_bytes"] == 0


def test_a_job_already_holding_memory_never_waits():
    """No hold-and-wait: two jobs each holding a charge and each wanting more
    would otherwise deadlock."""
    g = _gate(150)
    g.begin_job()
    g.charge("a", 10 * MB)             # 100 MB of 150
    t0 = time.monotonic()
    g.charge("b", 10 * MB)             # over budget, but this job holds: no wait
    assert time.monotonic() - t0 < 0.5
    assert g.stats()["overcommits"] == 1
    g.end_job()
    assert g.stats()["in_use_bytes"] == 0


def test_a_job_bigger_than_the_budget_runs_when_nothing_else_does():
    g = _gate(50)
    g.begin_job()
    t0 = time.monotonic()
    g.charge("huge", 100 * MB)         # 1 GB charge, 50 MB budget, nothing running
    assert time.monotonic() - t0 < 0.5
    g.end_job()


def test_the_pool_wrapper_releases_even_when_the_job_raises(monkeypatch):
    g = _gate(10_000)
    monkeypatch.setattr(mg, "gate", g)
    pool = mg.GatedThreadPoolExecutor(max_workers=1)

    def job():
        g.charge("a", 10 * MB)
        raise RuntimeError("boom")

    fut = pool.submit(job)
    try:
        fut.result(5)
    except RuntimeError:
        pass
    pool.shutdown(wait=True)
    assert g.stats()["in_use_bytes"] == 0


def test_switched_off():
    g = mg.MemoryGate(budget_bytes=1, enabled=False)
    g.begin_job()
    g.charge("a", 100 * MB)
    assert g.stats()["in_use_bytes"] == 0
    g.end_job()
