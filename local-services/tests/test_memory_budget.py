"""PERF-7: CPU-scaled DSP pool, one byte budget over every result cache, and
GET /diagnostics/memory."""
import gc

from khensu_services.common import executors, lru
from khensu_services.common.lru import BoundedCache


BIG = 64 * 2**30   # RAM that never caps the pool


def test_dsp_pool_scales_with_cpus_within_bounds():
    assert executors.dsp_worker_count(cpu_count=2, override="", ram_bytes=BIG) == 2
    assert executors.dsp_worker_count(cpu_count=8, override="", ram_bytes=BIG) == 4
    assert executors.dsp_worker_count(cpu_count=64, override="", ram_bytes=BIG) == 8
    assert executors.dsp_worker_count(cpu_count=8, override="3") == 3
    assert executors.dsp_worker_count(cpu_count=8, override="99") == 16
    assert executors.dsp_worker_count(cpu_count=8, override="junk", ram_bytes=BIG) == 4


def test_global_budget_evicts_the_least_recent_entry_across_caches(monkeypatch):
    monkeypatch.setenv("KHENSU_CACHE_BUDGET_MB", "1")
    # Empty the other caches' share so only these two count.
    for c in lru._live_caches():
        c.clear()
    a = BoundedCache(max_entries=10, max_bytes=None, name="a")  # no byte tracking
    b = BoundedCache(max_entries=10, name="b")
    c = BoundedCache(max_entries=10, name="c")
    blob = "x" * (400 * 1024)
    a["k"] = blob
    b["1"] = blob
    c["1"] = blob
    b["2"] = blob          # 4 x 400 KB tracked > 1 MB: c? no - b["1"] is oldest
    assert "1" not in b
    assert "1" in c and "2" in b
    assert "k" in a        # untracked caches are never touched
    total = sum(x.total_bytes for x in lru._live_caches())
    assert total <= 1024 * 1024 + 410 * 1024


def test_a_hit_refreshes_recency_for_the_global_budget(monkeypatch):
    monkeypatch.setenv("KHENSU_CACHE_BUDGET_MB", "1")
    for c in lru._live_caches():
        c.clear()
    b = BoundedCache(max_entries=10, name="b")
    c = BoundedCache(max_entries=10, name="c")
    blob = "x" * (400 * 1024)
    b["1"] = blob
    c["1"] = blob
    c["2"] = blob
    assert b.get("1") is not None   # b's entry is now the most recent
    c["3"] = blob
    assert "1" in b
    assert "1" not in c


def test_dead_caches_leave_the_registry():
    before = len(lru._live_caches())
    tmp = BoundedCache(name="tmp")
    assert len(lru._live_caches()) == before + 1
    del tmp
    gc.collect()
    assert len(lru._live_caches()) == before


def test_two_empty_caches_are_both_registered():
    x, y = BoundedCache(name="x"), BoundedCache(name="y")
    names = {s["name"] for s in lru.cache_stats()}
    assert {"x", "y"} <= names
    assert x == y == {}  # still plain dicts for comparisons


def test_diagnostics_memory_reports_caches_pools_and_rss(client):
    r = client.get("/diagnostics/memory")
    assert r.status_code == 200, r.text
    body = r.json()
    assert body["result_cache_budget_bytes"] > 0
    assert {p["name"] for p in body["pools"]} >= {"dsp", "long_job", "io", "separation", "llm"}
    assert body["cpu_count"] >= 1
    assert body["decoded_audio_budget_bytes"] > 0
    assert any(c["name"].startswith("file_analysis:") for c in body["result_caches"])
    assert body["rss_bytes"] is None or body["rss_bytes"] > 10 * 1024 * 1024


def test_dsp_workers_are_capped_by_ram():
    """2026-11-18: one request can peak near 1 GB, so a machine with little RAM
    runs fewer workers than its CPUs alone would give it."""
    from khensu_services.common import executors
    gb = 2**30
    assert executors.dsp_worker_count(cpu_count=16, override="", ram_bytes=32 * gb) == 8
    assert executors.dsp_worker_count(cpu_count=16, override="", ram_bytes=8 * gb) == 5
    assert executors.dsp_worker_count(cpu_count=16, override="", ram_bytes=6 * gb) == 3
    assert executors.dsp_worker_count(cpu_count=16, override="", ram_bytes=2 * gb) == 1
    # An explicit override still wins.
    assert executors.dsp_worker_count(cpu_count=16, override="6", ram_bytes=2 * gb) == 6
    # Unknown RAM: the CPU rule alone.
    assert executors.dsp_worker_count(cpu_count=16, override="", ram_bytes=0) == 8
    assert executors.physical_memory_bytes() is None or executors.physical_memory_bytes() > gb
