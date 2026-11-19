"""A tiny bounded LRU cache used for per-song analysis result caches.

Audit 2026-07-16, finding F2: ``_intake_cache`` in the intake orchestrator was
an unbounded dict that retained a full analysis bundle per (file, options) key
for the life of the process — the same result the job payload already holds, so
it also double-retained. This caps it. Insertion- and access-ordered so the
least-recently-used entry is the one evicted.

Audit 2026-07-18, ARCH-04: entry-count alone doesn't bound memory when a
single entry (a large spectrogram/heatmap grid) can itself be large — a cache
at its 32-entry cap could still be tens of megabytes. Byte-budget eviction is
on by default (``max_bytes``, ~8MB) alongside the entry cap; pass
``max_bytes=None`` to opt out for a cache that's known to hold small values.

Thread-safe: every mutating/recency-bumping operation holds an internal
``RLock`` (these caches are shared by concurrent worker threads).

Subclasses ``dict`` so existing access (``cache[key] = v``, ``cache.pop(key,
None)``) keeps working; ``get`` is overridden only to refresh recency.
"""
from __future__ import annotations

import itertools
import json
import os
import sys
import threading
import weakref
from typing import Any, Generic, TypeVar

_K = TypeVar("_K")
_V = TypeVar("_V")

_DEFAULT_MAX_BYTES = 8 * 1024 * 1024  # ~8MB

# PERF-7: one byte budget over every BoundedCache in the process. Each cache
# keeps its own entry and byte caps; past the global budget the least recently
# used entry of whichever cache holds it goes, so forty 8 MB caches cannot add
# up to 320 MB. ``KHENSU_CACHE_BUDGET_MB`` overrides the default.
_GLOBAL_BUDGET_DEFAULT = 192 * 1024 * 1024
# id -> weak reference (not a WeakSet: these are dicts, and two empty caches
# compare equal).
_registry: "dict[int, weakref.ref]" = {}
_registry_lock = threading.Lock()
_tick = itertools.count()


def global_budget_bytes() -> int:
    raw = os.environ.get("KHENSU_CACHE_BUDGET_MB")
    if raw:
        try:
            return max(1, int(raw)) * 1024 * 1024
        except ValueError:
            pass
    return _GLOBAL_BUDGET_DEFAULT


def cache_stats() -> list[dict[str, Any]]:
    """One row per live cache: name, entries, estimated bytes."""
    caches = _live_caches()
    return [{"name": c.name, "entries": len(c), "bytes": c.total_bytes} for c in caches]


def _live_caches() -> "list[BoundedCache]":
    with _registry_lock:
        refs = list(_registry.values())
    return [c for c in (r() for r in refs) if c is not None]


def _enforce_global_budget() -> None:
    budget = global_budget_bytes()
    with _registry_lock:
        caches = [c for c in (r() for r in list(_registry.values())) if c is not None]
        total = sum(c._total_bytes for c in caches)
        while total > budget:
            oldest, oldest_tick = None, None
            for c in caches:
                t = c._oldest_tick()
                if t is not None and len(c) > 1 and (oldest_tick is None or t < oldest_tick):
                    oldest, oldest_tick = c, t
            if oldest is None:
                break  # every cache is down to its newest entry
            before = oldest._total_bytes
            oldest._evict_lru()
            total -= before - oldest._total_bytes


class BoundedCache(dict, Generic[_K, _V]):
    def __init__(self, *, max_entries: int = 32, max_bytes: int | None = _DEFAULT_MAX_BYTES,
                 name: str | None = None) -> None:
        super().__init__()
        self._max = max(1, int(max_entries))
        self._max_bytes = int(max_bytes) if max_bytes is not None else None
        self._sizes: dict[Any, int] = {}
        self._ticks: dict[Any, int] = {}
        self._total_bytes = 0
        self._lock = threading.RLock()
        if name is None:
            frame = sys._getframe(1)
            name = f"{frame.f_globals.get('__name__', '?')}:{frame.f_lineno}"
        self.name = name
        key = id(self)
        with _registry_lock:
            _registry[key] = weakref.ref(self, lambda _r, k=key: _registry.pop(k, None))

    def _oldest_tick(self) -> int | None:
        with self._lock:
            if not self:
                return None
            return self._ticks.get(next(iter(self)))

    def _evict_lru(self) -> None:
        with self._lock:
            if self:
                self._evict_oldest()

    @staticmethod
    def _estimate_bytes(value: Any) -> int:
        """Cheap proxy for a value's real memory footprint: these caches hold
        JSON-serializable response dicts (grids of floats), so the JSON
        encoding's length tracks actual size closely enough for eviction
        purposes without needing a real (slow, recursive) memory profiler."""
        try:
            return len(json.dumps(value, default=str))
        except (TypeError, ValueError):
            return sys.getsizeof(value)

    def __setitem__(self, key: _K, value: _V) -> None:
        # Size estimate (a JSON encode) is done outside the lock.
        size = self._estimate_bytes(value) if self._max_bytes is not None else 0
        with self._lock:
            if key in self:
                self._total_bytes -= self._sizes.pop(key, 0)
                super().__delitem__(key)  # re-insert at the end (most recent)
            super().__setitem__(key, value)
            self._sizes[key] = size
            self._ticks[key] = next(_tick)
            self._total_bytes += size

            while len(self) > self._max:
                self._evict_oldest()
            if self._max_bytes is not None:
                # Always keep at least the entry just inserted, even if it alone
                # exceeds the byte budget — an oversized single entry should
                # evict everything *else*, not leave the cache permanently empty.
                while self._total_bytes > self._max_bytes and len(self) > 1:
                    self._evict_oldest()
        if size:
            _enforce_global_budget()

    def _evict_oldest(self) -> None:
        # Caller holds self._lock.
        oldest = next(iter(self))  # dicts preserve insertion order
        self._total_bytes -= self._sizes.pop(oldest, 0)
        self._ticks.pop(oldest, None)
        super().__delitem__(oldest)

    def get(self, key, default=None):  # type: ignore[override]
        with self._lock:
            if key in self:
                value = super().__getitem__(key)
                size = self._sizes.pop(key, 0)
                super().__delitem__(key)
                super().__setitem__(key, value)  # bump recency on hit
                self._sizes[key] = size
                self._ticks[key] = next(_tick)
                return value
            return default

    def pop(self, key, *args):  # type: ignore[override]
        with self._lock:
            self._total_bytes -= self._sizes.pop(key, 0)
            self._ticks.pop(key, None)
            return super().pop(key, *args)

    def __delitem__(self, key) -> None:
        with self._lock:
            self._total_bytes -= self._sizes.pop(key, 0)
            self._ticks.pop(key, None)
            super().__delitem__(key)

    def clear(self) -> None:
        with self._lock:
            self._sizes.clear()
            self._ticks.clear()
            self._total_bytes = 0
            super().clear()

    @property
    def total_bytes(self) -> int:
        """Estimated total size of all cached values, in bytes."""
        return self._total_bytes


def file_cache_key(paths: Any, *params: Any) -> tuple | None:
    """Result-cache key for an analysis of the file(s) ``paths`` (a path, a
    list of paths, or a ``{name: path}`` dict) plus every request parameter
    that changes the result. Each file contributes (path, mtime_ns, size), so
    a file re-rendered in place is never served a stale result - the same
    pattern router_artifacts_scan uses. None when a file can't be stat'd
    (the caller then simply doesn't cache)."""
    import os

    if isinstance(paths, str):
        items: list[tuple[Any, str]] = [("", paths)]
    elif isinstance(paths, dict):
        items = sorted(paths.items())
    else:
        items = [(i, p) for i, p in enumerate(paths)]
    parts: list[Any] = []
    for name, path in items:
        try:
            st = os.stat(path)
        except OSError:
            return None
        parts.append((name, os.path.normcase(os.path.abspath(path)), st.st_mtime_ns, st.st_size))
    frozen = json.dumps(params, sort_keys=True, default=str)
    return (tuple(parts), frozen)
