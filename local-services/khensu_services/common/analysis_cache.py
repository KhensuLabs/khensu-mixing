"""Content-addressed, persistent analysis-result cache (CE-PLT-2, PERF-3/PERF-7).

Reopening a session, re-running a scan or asking for the same sections twice
used to repeat the whole decode + analysis: the per-router ``BoundedCache``
instances live in memory only and die with the service. This cache keeps
JSON-safe analyzer results on disk under ``<config dir>/analysis_cache`` so the
second request for the same audio is a file read.

**Key.** ``sha256`` over:

* the sha256 of every input file's *raw bytes* (stream-hashed in 1 MiB chunks,
  no audio decode - so hashing never doubles the decode work it guards),
* the analyzer name and the analyzer's own version string, declared in
  ``common/analysis_versions`` (FIX-15: bump it when the analyzer's output
  changes, and only that analyzer's entries stop matching),
* ``SERVICE_VERSION`` (a release that changes any analyzer invalidates every
  entry rather than risk serving numbers a newer analyzer would not produce),
* the canonical JSON of every request parameter that changes the output.

A file re-rendered in place has new bytes, so it gets a new key; the path is
deliberately not part of the key, so a copy or a moved file still hits.

**Store.** One ``<analyzer>__<key>.json`` per entry under a two-character fan-
out directory. Writes go to a unique temp file in the same directory followed
by ``os.replace`` (atomic on NTFS and POSIX), so a reader in another thread or
another process sees either the old entry or the new one, never a torn one.
Recency is the file's mtime (bumped on every hit), which every process sees,
so LRU eviction is consistent across the desktop-spawned service and a CLI
run sharing the same directory.

**Budget.** One global byte budget for the whole store (default 512 MiB,
``KHENSU_ANALYSIS_CACHE_MB`` overrides). Past it the least-recently-used
entries are deleted down to 90 % of the budget. ``KHENSU_ANALYSIS_CACHE=0``
turns the cache off; ``KHENSU_ANALYSIS_CACHE_DIR`` moves it (tests use this).

Never raises into a caller: any I/O problem is a cache miss.
"""
from __future__ import annotations

import hashlib
import json
import logging
import math
import os
import re
import threading
import time
import uuid
from collections import OrderedDict
from collections.abc import Callable, Iterable
from pathlib import Path
from typing import Any, TypeVar

from khensu_services.common.analysis_versions import version_of
from khensu_services.common.config import config_dir
from khensu_services.common.single_flight import SingleFlight

_log = logging.getLogger(__name__)

_T = TypeVar("_T")

#: Bump when the on-disk entry format changes.
CACHE_FORMAT_VERSION = 1
DEFAULT_BUDGET_MB = 512
_LOW_WATERMARK = 0.9
_HASH_CHUNK = 1 << 20
#: Every Nth put re-scans the directory so another process's writes count.
_RESCAN_EVERY = 64
_SAFE_NAME = re.compile(r"[^A-Za-z0-9_.-]+")


# --------------------------------------------------------------------------- #
# Content hashing
# --------------------------------------------------------------------------- #

_hash_memo: OrderedDict[tuple, str] = OrderedDict()
_hash_lock = threading.Lock()
_HASH_MEMO_MAX = 4096
_FRESH_NS = 2_000_000_000


def content_hash(path: str | os.PathLike[str]) -> str:
    """sha256 hex of the file's bytes, memoised per (path, mtime, size, inode)
    so a file hashed once in this process is not re-read until it changes.
    Raises OSError when the file can't be read."""
    p = os.fspath(path)
    st = os.stat(p)
    memo_key = (os.path.normcase(os.path.abspath(p)), st.st_mtime_ns, st.st_size, st.st_ino)
    with _hash_lock:
        hit = _hash_memo.get(memo_key)
        if hit is not None:
            _hash_memo.move_to_end(memo_key)
            return hit
    h = hashlib.sha256()
    with open(p, "rb") as f:
        while True:
            chunk = f.read(_HASH_CHUNK)
            if not chunk:
                break
            h.update(chunk)
    digest = h.hexdigest()
    # A file written in the last couple of seconds may be rewritten again
    # within the filesystem's timestamp granularity (NTFS mtime moves in
    # ~1-16 ms ticks) at the same size: don't memoise it yet.
    if time.time_ns() - st.st_mtime_ns < _FRESH_NS:
        return digest
    with _hash_lock:
        _hash_memo[memo_key] = digest
        while len(_hash_memo) > _HASH_MEMO_MAX:
            _hash_memo.popitem(last=False)
    return digest


def canonical_params(value: Any) -> Any:
    """A JSON-stable form of request parameters: dict keys sorted (by
    ``json.dumps(sort_keys=True)``), tuples as lists, pydantic models dumped,
    paths as strings, non-finite floats spelled out (JSON has no NaN)."""
    if hasattr(value, "model_dump"):
        return canonical_params(value.model_dump(mode="json"))
    if isinstance(value, dict):
        return {str(k): canonical_params(v) for k, v in value.items()}
    if isinstance(value, (list, tuple)):
        return [canonical_params(v) for v in value]
    if isinstance(value, float):
        if math.isnan(value):
            return "NaN"
        if math.isinf(value):
            return "Infinity" if value > 0 else "-Infinity"
        return value
    if isinstance(value, (str, int, bool)) or value is None:
        return value
    if isinstance(value, os.PathLike):
        return os.fspath(value)
    return repr(value)


def make_key(content_hashes: Iterable[str], analyzer: str, version: str, params: Any = None) -> str:
    from khensu_services.version import SERVICE_VERSION

    payload = {
        "f": CACHE_FORMAT_VERSION,
        "c": list(content_hashes),
        "a": analyzer,
        "v": str(version),
        "s": SERVICE_VERSION,
        "p": canonical_params(params),
    }
    blob = json.dumps(payload, sort_keys=True, separators=(",", ":"), ensure_ascii=True)
    return hashlib.sha256(blob.encode("ascii")).hexdigest()


# --------------------------------------------------------------------------- #
# Store
# --------------------------------------------------------------------------- #


def _json_default(obj: Any) -> Any:
    """numpy scalars / arrays in analyzer dicts (np.bool_, np.int64, ...)."""
    if hasattr(obj, "tolist"):
        return obj.tolist()
    if hasattr(obj, "item"):
        return obj.item()
    raise TypeError(f"not JSON-serialisable: {type(obj).__name__}")


def _safe_analyzer(name: str) -> str:
    return _SAFE_NAME.sub("_", name)[:64] or "analyzer"


class AnalysisCache:
    """The on-disk store. Thread-safe; safe to share a directory between
    processes (atomic replace + mtime recency + idempotent eviction)."""

    def __init__(self, root: Path, max_bytes: int) -> None:
        self.root = Path(root)
        self.max_bytes = max(1, int(max_bytes))
        self._lock = threading.Lock()
        self._approx_bytes: int | None = None  # lazily scanned
        self._puts_since_scan = 0
        self.hits = 0
        self.misses = 0
        self.puts = 0
        self.evictions = 0

    # -- paths ---------------------------------------------------------------
    def _path(self, analyzer: str, key: str) -> Path:
        return self.root / key[:2] / f"{_safe_analyzer(analyzer)}__{key}.json"

    def _entries(self) -> list[tuple[Path, int, float]]:
        out: list[tuple[Path, int, float]] = []
        try:
            subdirs = list(self.root.iterdir())
        except OSError:
            return out
        for sub in subdirs:
            if not sub.is_dir():
                continue
            try:
                files = list(sub.iterdir())
            except OSError:
                continue
            for f in files:
                if f.suffix != ".json":
                    continue
                try:
                    st = f.stat()
                except OSError:
                    continue
                out.append((f, st.st_size, st.st_mtime))
        return out

    # -- read ----------------------------------------------------------------
    def get(self, analyzer: str, key: str) -> Any | None:
        path = self._path(analyzer, key)
        try:
            with open(path, encoding="utf-8") as f:
                entry = json.load(f)
        except FileNotFoundError:
            with self._lock:
                self.misses += 1
            return None
        except (OSError, ValueError):
            # Unreadable or (impossible with atomic writes, but) corrupt:
            # a miss, and drop it so the next put starts clean.
            with self._lock:
                self.misses += 1
            try:
                path.unlink()
            except OSError:
                pass
            return None
        if not isinstance(entry, dict) or entry.get("format") != CACHE_FORMAT_VERSION \
                or entry.get("key") != key:
            with self._lock:
                self.misses += 1
            return None
        try:
            os.utime(path, None)  # LRU recency, visible to every process
        except OSError:
            pass
        with self._lock:
            self.hits += 1
        return entry.get("value")

    # -- write ---------------------------------------------------------------
    def put(self, analyzer: str, key: str, value: Any) -> bool:
        """Store ``value`` (must be JSON-serialisable). False when it isn't,
        or when the write failed; the caller simply carries on uncached."""
        entry = {"format": CACHE_FORMAT_VERSION, "key": key, "analyzer": analyzer,
                 "created": time.time(), "value": value}
        try:
            blob = json.dumps(entry, ensure_ascii=False, separators=(",", ":"),
                              default=_json_default)
        except (TypeError, ValueError):
            return False
        data = blob.encode("utf-8")
        if len(data) > self.max_bytes:
            return False
        path = self._path(analyzer, key)
        tmp = path.with_name(f".{path.name}.{os.getpid()}.{uuid.uuid4().hex}.tmp")
        try:
            path.parent.mkdir(parents=True, exist_ok=True)
            with open(tmp, "wb") as f:
                f.write(data)
            replaced = False
            for attempt in range(5):
                try:
                    os.replace(tmp, path)
                    replaced = True
                    break
                except PermissionError:
                    # Windows: a reader in another process holds the target
                    # open for the few microseconds of its read. Retry.
                    time.sleep(0.01 * (attempt + 1))
            if not replaced:
                tmp.unlink(missing_ok=True)
                return False
        except OSError:
            try:
                tmp.unlink(missing_ok=True)
            except OSError:
                pass
            return False

        with self._lock:
            self.puts += 1
            self._puts_since_scan += 1
            need_scan = self._approx_bytes is None or self._puts_since_scan >= _RESCAN_EVERY
        if need_scan:
            self._rescan()
        else:
            with self._lock:
                self._approx_bytes = (self._approx_bytes or 0) + len(data)
        with self._lock:
            over = (self._approx_bytes or 0) > self.max_bytes
        if over:
            self.evict_to(int(self.max_bytes * _LOW_WATERMARK))
        return True

    def _rescan(self) -> int:
        total = sum(size for _, size, _ in self._entries())
        with self._lock:
            self._approx_bytes = total
            self._puts_since_scan = 0
        return total

    def evict_to(self, target_bytes: int) -> int:
        """Delete least-recently-used entries until the store is at most
        ``target_bytes``. Returns the number deleted. Safe to run in two
        processes at once (a vanished file is simply skipped)."""
        entries = self._entries()
        total = sum(size for _, size, _ in entries)
        removed = 0
        if total > target_bytes:
            entries.sort(key=lambda e: e[2])  # oldest mtime first
            for path, size, _ in entries:
                if total <= target_bytes:
                    break
                try:
                    path.unlink()
                except FileNotFoundError:
                    total -= size  # another process got it first
                    continue
                except OSError:
                    continue      # open in a reader right now: skip it
                total -= size
                removed += 1
        with self._lock:
            self._approx_bytes = total
            self._puts_since_scan = 0
            self.evictions += removed
        return removed

    # -- admin ---------------------------------------------------------------
    def stats(self) -> dict[str, Any]:
        entries = self._entries()
        total = sum(size for _, size, _ in entries)
        per: dict[str, dict[str, int]] = {}
        for path, size, _ in entries:
            name = path.name.split("__", 1)[0]
            slot = per.setdefault(name, {"entries": 0, "bytes": 0})
            slot["entries"] += 1
            slot["bytes"] += size
        with self._lock:
            self._approx_bytes = total
            return {
                "enabled": True,
                "directory": str(self.root),
                "entries": len(entries),
                "bytes": total,
                "budget_bytes": self.max_bytes,
                "hits": self.hits,
                "misses": self.misses,
                "puts": self.puts,
                "evictions": self.evictions,
                "analyzers": [{"analyzer": k, **v} for k, v in sorted(per.items())],
            }

    def clear(self) -> int:
        removed = 0
        for path, _, _ in self._entries():
            try:
                path.unlink()
                removed += 1
            except OSError:
                continue
        with self._lock:
            self._approx_bytes = 0
            self._puts_since_scan = 0
        return removed


# --------------------------------------------------------------------------- #
# Process-wide instance
# --------------------------------------------------------------------------- #

_instance: AnalysisCache | None = None
_instance_cfg: tuple[str, int] | None = None
_instance_lock = threading.Lock()


def enabled() -> bool:
    return os.environ.get("KHENSU_ANALYSIS_CACHE", "1").strip().lower() not in ("0", "false", "off", "no")


def default_root() -> Path:
    override = os.environ.get("KHENSU_ANALYSIS_CACHE_DIR", "").strip()
    return Path(override) if override else config_dir() / "analysis_cache"


def budget_bytes() -> int:
    raw = os.environ.get("KHENSU_ANALYSIS_CACHE_MB", "").strip()
    try:
        mb = float(raw) if raw else float(DEFAULT_BUDGET_MB)
    except ValueError:
        mb = float(DEFAULT_BUDGET_MB)
    return max(1, int(mb * 1024 * 1024))


def get_cache() -> AnalysisCache:
    """The shared store. Re-created when the directory or budget setting
    changes (tests point it at a per-test directory)."""
    global _instance, _instance_cfg
    cfg = (str(default_root()), budget_bytes())
    with _instance_lock:
        if _instance is None or _instance_cfg != cfg:
            _instance = AnalysisCache(Path(cfg[0]), cfg[1])
            _instance_cfg = cfg
        return _instance


def key_for(paths: str | os.PathLike[str] | Iterable[str | os.PathLike[str]],
            analyzer: str, version: str, params: Any = None) -> str | None:
    """Cache key for ``analyzer`` over the file(s) ``paths``; None when the
    cache is off or a file can't be hashed (the caller then just computes)."""
    if not enabled():
        return None
    items = [paths] if isinstance(paths, (str, os.PathLike)) else list(paths)
    try:
        hashes = [content_hash(p) for p in items]
    except OSError:
        return None
    return make_key(hashes, analyzer, version, params)


def key_for_analyzer(paths: str | os.PathLike[str] | Iterable[str | os.PathLike[str]],
                     analyzer: str, params: Any = None) -> str | None:
    """``key_for`` at the analyzer's declared version (FIX-15) - what service
    code uses. Raises ``UndeclaredAnalyzerError`` for a name that is not in
    ``common/analysis_versions``."""
    return key_for(paths, analyzer, version_of(analyzer), params)


def lookup(analyzer: str, key: str | None) -> Any | None:
    if key is None:
        return None
    try:
        return get_cache().get(analyzer, key)
    except Exception:  # never let the cache break an analysis
        _log.debug("analysis cache read failed", exc_info=True)
        return None


def store(analyzer: str, key: str | None, value: Any) -> bool:
    if key is None:
        return False
    try:
        return get_cache().put(analyzer, key, value)
    except Exception:
        _log.debug("analysis cache write failed", exc_info=True)
        return False


_flights: SingleFlight[Any] = SingleFlight()


def cached_call(paths: Any, analyzer: str, version: str, params: Any,
                compute: Callable[[], _T]) -> _T:
    """Blocking: return the cached result of ``analyzer`` over ``paths`` with
    ``params``, or run ``compute()`` and store what it returns. The explicit-
    version primitive: service code goes through ``persistent_call``, which
    takes the version from ``common/analysis_versions``."""
    key = key_for(paths, analyzer, version, params)
    if key is None:                 # cache off, or the file cannot be hashed
        return compute()

    def _fill() -> _T:
        value = compute()
        store(analyzer, key, value)
        return value

    # Single flight (FIX-16): callers that miss the same entry together wait
    # for the first one instead of each running the scan. No lock is held
    # while it runs, so a scan may read another entry; a failed scan stores
    # nothing and the waiters try again.
    return _flights.do((analyzer, key), _fill, probe=lambda: lookup(analyzer, key))


def persistent_call(paths: Any, analyzer: str, params: Any,
                    fn: Callable[..., _T], *args: Any) -> _T:
    """Blocking: ``fn(*args)`` through the disk tier (PERF-3), for a scan
    whose result depends only on the file bytes and ``params``. Reopening a
    session, or restarting the app, then reads the stored result instead of
    scanning again. Keyed on the content hash, SERVICE_VERSION and the
    analyzer's declared version (``common/analysis_versions``, FIX-15), so an
    edited file, a new service or a corrected analyzer never sees a stale
    entry. An undeclared ``analyzer`` raises ``UndeclaredAnalyzerError``."""
    return cached_call(paths, analyzer, version_of(analyzer), params, lambda: fn(*args))


def stats() -> dict[str, Any]:
    if not enabled():
        return {"enabled": False, "directory": str(default_root()), "entries": 0, "bytes": 0,
                "budget_bytes": budget_bytes(), "hits": 0, "misses": 0, "puts": 0,
                "evictions": 0, "analyzers": []}
    return get_cache().stats()


def clear() -> int:
    return get_cache().clear()
