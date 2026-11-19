"""Byte-size budget on BoundedCache (2026-07-18 audit ARCH-04 remainder).

Entry-count alone doesn't bound memory when a single entry (a large
spectrogram/heatmap grid) can itself be large — a cache at its entry cap
could still be tens of megabytes. These cover the byte-budget eviction added
alongside the existing entry-count cap.
"""
from khensu_services.common.lru import BoundedCache


def test_total_bytes_starts_at_zero():
    cache = BoundedCache(max_entries=10)
    assert cache.total_bytes == 0


def test_total_bytes_tracks_inserted_values():
    cache = BoundedCache(max_entries=10)
    cache["a"] = {"x": 1}
    assert cache.total_bytes > 0


def test_evicts_oldest_once_byte_budget_is_exceeded():
    cache = BoundedCache(max_entries=100, max_bytes=50)
    cache["a"] = {"data": "x" * 40}
    assert "a" in cache
    cache["b"] = {"data": "y" * 40}
    # Combined exceeds the 50-byte budget - "a" (oldest) must be evicted even
    # though the entry-count cap (100) is nowhere close.
    assert "a" not in cache
    assert "b" in cache


def test_a_single_oversized_entry_is_not_evicted_by_itself():
    cache = BoundedCache(max_entries=100, max_bytes=10)
    cache["a"] = {"data": "x" * 1000}  # far exceeds the 10-byte budget alone
    # Must not evict the only entry just inserted — an oversized single
    # value should still be retrievable, not silently dropped.
    assert "a" in cache


def test_max_bytes_none_disables_the_byte_budget():
    cache = BoundedCache(max_entries=100, max_bytes=None)
    for i in range(50):
        cache[str(i)] = {"data": "x" * 10_000}
    assert len(cache) == 50  # only the entry-count cap applies
    assert cache.total_bytes == 0  # size tracking is skipped entirely


def test_default_byte_budget_is_applied_when_unspecified():
    cache = BoundedCache(max_entries=1000)
    for i in range(1000):
        cache[str(i)] = {"data": "x" * 100_000}  # ~100KB each -> ~100MB total
    # The default ~8MB budget must have evicted most entries long before
    # hitting the 1000-entry cap.
    assert len(cache) < 1000
    assert cache.total_bytes <= 8 * 1024 * 1024 + 200_000  # small slack for the last insert


def test_pop_keeps_total_bytes_consistent():
    cache = BoundedCache(max_entries=10)
    cache["a"] = {"x": 1}
    before = cache.total_bytes
    assert before > 0
    cache.pop("a")
    assert cache.total_bytes == 0


def test_delitem_keeps_total_bytes_consistent():
    cache = BoundedCache(max_entries=10)
    cache["a"] = {"x": 1}
    del cache["a"]
    assert cache.total_bytes == 0


def test_clear_resets_total_bytes():
    cache = BoundedCache(max_entries=10)
    cache["a"] = {"x": 1}
    cache["b"] = {"y": 2}
    cache.clear()
    assert cache.total_bytes == 0
    assert len(cache) == 0


def test_get_does_not_double_count_bytes_on_recency_bump():
    cache = BoundedCache(max_entries=10)
    cache["a"] = {"x": 1}
    before = cache.total_bytes
    cache.get("a")
    cache.get("a")
    assert cache.total_bytes == before


def test_overwriting_a_key_updates_its_tracked_size():
    cache = BoundedCache(max_entries=10, max_bytes=1000)
    cache["a"] = {"x": 1}
    small = cache.total_bytes
    cache["a"] = {"x": "y" * 200}
    assert cache.total_bytes > small
