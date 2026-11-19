"""Durable, bounded security event log (2026-07-18 audit SEC-07).

Auth failures, gate-bypass attempts, model-hash mismatches, and job-admission
429s previously left no trace beyond an ordinary log line (or nothing at
all): there was no way to see "this machine has had 40 failed-auth attempts
in the last hour" without grepping raw logs. This module gives every one of
those a single, structured, append-only sink that is:

* Bounded on disk (rotates at a fixed size, keeps one backup) so it can never
  grow without limit the way an unrotated log file could.
* Bounded in memory (a fixed-size ring buffer) for the /diagnostics endpoint
  to read cheaply without re-parsing the file on every request.
* Redacted by construction: callers pass a `kind` (from a fixed vocabulary)
  and a short, pre-redacted `detail` string. This module never accepts or
  logs bearer tokens, API keys, prompts, or full file paths — callers are
  responsible for not passing them, and every detail is additionally
  truncated to a bounded length as a second line of defense.
* Never raises: a logging failure must not break the request that triggered
  it, matching the "never raises" contract other common/ modules use.
"""
from __future__ import annotations

import json
import threading
import time
from collections import deque
from pathlib import Path
from typing import Any

from khensu_services.common.config import config_dir

_MAX_DETAIL_CHARS = 200
_MAX_MEMORY_EVENTS = 500
_MAX_LOG_FILE_BYTES = 1_000_000  # rotate at ~1MB
# Retention window for reads: an event older than this is treated as expired
# even if the count-bounded ring buffer hasn't evicted it yet (a quiet
# service could otherwise keep a months-old event looking "recent" forever).
_RETENTION_SEC = 30 * 24 * 3600  # 30 days

# A count at/above this within ANOMALY_WINDOW_SEC is flagged as an anomaly in
# the diagnostics summary — repeated auth failures/hash mismatches/gate
# blocks in a short window are the actual signal the audit asked to surface,
# not just a raw event list a human has to eyeball for patterns.
ANOMALY_WINDOW_SEC = 3600  # 1 hour
ANOMALY_THRESHOLD = 5

_lock = threading.Lock()
_memory_events: deque[dict[str, Any]] = deque(maxlen=_MAX_MEMORY_EVENTS)

# Fixed vocabulary — an unrecognized kind is still recorded (fail open on
# logging itself, since refusing to record an unexpected event is worse than
# recording it under "other"), but keeping call sites to this list documents
# what the diagnostics view actually covers.
KIND_AUTH_FAILURE = "auth_failure"
KIND_ONLINE_GATE_BLOCKED = "online_gate_blocked"
KIND_MODEL_HASH_MISMATCH = "model_hash_mismatch"
KIND_MODEL_UNAVAILABLE = "model_unavailable"
KIND_JOB_ADMISSION_REJECTED = "job_admission_rejected"
KIND_UNHANDLED_EXCEPTION = "unhandled_exception"
KIND_LLM_BUDGET_EXHAUSTED = "llm_budget_exhausted"
KIND_GUARDRAIL_OVERRIDE = "guardrail_override"


def _log_path() -> Path:
    return config_dir() / "security_events.log"


def _redact(detail: str) -> str:
    detail = detail.replace("\n", " ").replace("\r", " ").strip()
    if len(detail) > _MAX_DETAIL_CHARS:
        detail = detail[:_MAX_DETAIL_CHARS] + "…"
    return detail


def record_event(kind: str, detail: str = "") -> None:
    """Append a security-relevant event. Never raises."""
    try:
        entry = {
            "ts": time.time(),
            "kind": str(kind),
            "detail": _redact(str(detail)),
        }
        with _lock:
            _memory_events.append(entry)
            _append_to_file(entry)
    except Exception:
        pass  # logging a security event must never break the caller's request


def _append_to_file(entry: dict[str, Any]) -> None:
    path = _log_path()
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        if path.exists() and path.stat().st_size > _MAX_LOG_FILE_BYTES:
            backup = path.with_suffix(".log.1")
            backup.unlink(missing_ok=True)
            path.rename(backup)
        with open(path, "a", encoding="utf-8") as f:
            f.write(json.dumps(entry, sort_keys=True) + "\n")
    except OSError:
        pass  # disk full / permissions — the in-memory ring buffer still has it


def recent_events(limit: int = 100) -> list[dict[str, Any]]:
    """Most recent events, newest first, capped at `limit` (itself capped to
    the in-memory buffer size — this never reads the on-disk file). Events
    older than the retention window are excluded even if still physically
    present in the ring buffer."""
    limit = max(0, min(int(limit), _MAX_MEMORY_EVENTS))
    cutoff = time.time() - _RETENTION_SEC
    with _lock:
        events = [e for e in _memory_events if e["ts"] >= cutoff]
    return list(reversed(events))[:limit]


def anomaly_counts(window_sec: float = ANOMALY_WINDOW_SEC) -> dict[str, int]:
    """Per-kind event counts within the last `window_sec` seconds, for every
    kind that has reached ANOMALY_THRESHOLD — e.g. ``{"auth_failure": 12}``
    surfaces "12 failed-auth attempts in the last hour" directly instead of
    requiring a human to count a raw event list."""
    cutoff = time.time() - window_sec
    counts: dict[str, int] = {}
    with _lock:
        for e in _memory_events:
            if e["ts"] >= cutoff:
                counts[e["kind"]] = counts.get(e["kind"], 0) + 1
    return {kind: n for kind, n in counts.items() if n >= ANOMALY_THRESHOLD}


def clear_memory_events_for_testing() -> None:
    with _lock:
        _memory_events.clear()
