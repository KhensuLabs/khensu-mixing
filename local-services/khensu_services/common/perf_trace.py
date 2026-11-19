"""Opt-in Chrome-trace performance spans (CE-ENG-4).

``with span("decode", file=path): ...`` records a complete ("X") event with
the wall-clock start, duration, thread and arguments. Spans nest naturally
(Chrome's trace viewer / Perfetto draw them as a flame graph per thread).

Recording is **off** unless the ``diagnostics.perf_traces`` setting is on
(Settings > Diagnostics, persisted in settings.json) or ``KHENSU_PERF_TRACES``
is ``1``. Off, ``span`` costs one cached settings lookup. On, events are kept
in a bounded in-memory buffer and written atomically to
``<diagnostics dir>/traces/trace.json`` whenever a top-level span ends, so the
file is always a loadable trace of the most recent work. Open it in
``chrome://tracing`` or https://ui.perfetto.dev (a local file load - nothing is
uploaded by the service, ever).
"""
from __future__ import annotations

import json
import os
import threading
import time
from collections import deque
from collections.abc import Iterator
from contextlib import contextmanager
from pathlib import Path
from typing import Any

#: Most recent events kept (a long intake produces a few hundred).
MAX_EVENTS = 20_000

_events: deque[dict[str, Any]] = deque(maxlen=MAX_EVENTS)
_lock = threading.Lock()
_local = threading.local()
_T0_NS = time.perf_counter_ns()
_EPOCH_US = time.time() * 1e6


def enabled() -> bool:
    env = os.environ.get("KHENSU_PERF_TRACES", "").strip().lower()
    if env in ("1", "true", "on", "yes"):
        return True
    if env in ("0", "false", "off", "no"):
        return False
    try:
        from khensu_services.common.online import load_settings
        diag = load_settings().get("diagnostics", {})
        return isinstance(diag, dict) and diag.get("perf_traces") is True
    except Exception:
        return False


def trace_path() -> Path:
    from khensu_services.common.crash_log import traces_dir
    return traces_dir() / "trace.json"


def _now_us() -> float:
    return _EPOCH_US + (time.perf_counter_ns() - _T0_NS) / 1000.0


def _safe_args(args: dict[str, Any]) -> dict[str, Any]:
    out: dict[str, Any] = {}
    for k, v in args.items():
        out[k] = v if isinstance(v, (str, int, float, bool)) or v is None else repr(v)
    return out


@contextmanager
def span(name: str, cat: str = "analysis", **args: Any) -> Iterator[None]:
    """Record ``name`` as a Chrome-trace complete event when traces are on."""
    if not enabled():
        yield
        return
    depth = getattr(_local, "depth", 0)
    _local.depth = depth + 1
    start = _now_us()
    try:
        yield
    finally:
        dur = _now_us() - start
        _local.depth = depth
        event = {"name": name, "cat": cat, "ph": "X", "ts": round(start, 1),
                 "dur": round(dur, 1), "pid": os.getpid(), "tid": threading.get_ident(),
                 "args": _safe_args(args)}
        with _lock:
            _events.append(event)
        if depth == 0:
            flush()


def events() -> list[dict[str, Any]]:
    with _lock:
        return list(_events)


def clear() -> None:
    with _lock:
        _events.clear()


def flush(path: Path | None = None) -> Path | None:
    """Write the buffer as a Chrome trace (atomic replace). Never raises."""
    target = path if path is not None else trace_path()
    with _lock:
        snapshot = list(_events)
    meta = [{"name": "process_name", "ph": "M", "pid": os.getpid(),
             "args": {"name": "khensu-service"}}]
    try:
        target.parent.mkdir(parents=True, exist_ok=True)
        tmp = target.with_name(f".{target.name}.{os.getpid()}.{threading.get_ident()}.tmp")
        tmp.write_text(json.dumps({"traceEvents": meta + snapshot, "displayTimeUnit": "ms"}),
                       encoding="utf-8")
        os.replace(tmp, target)
        return target
    except OSError:
        return None
