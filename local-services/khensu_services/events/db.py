"""Append-only provenance event store (SQLite).

Thread-safe via check_same_thread=False + a module mutex, mirroring
feedback/db.py. One row per event; payload is free-form JSON so producers can
attach recommendation ids, parameter sets, and result file paths without a
schema migration each time.
"""
from __future__ import annotations

import json
import os
import sqlite3
import threading
import time
import uuid
from datetime import datetime, timedelta, timezone

# ENG-8: the table comes from migration 011 itself (generated into
# common/schema_sql.py), not a hand-kept copy of its DDL.
from khensu_services.common.schema_sql import MIGRATIONS as _MIGRATIONS  # noqa: E402

# Recognised event types (open set; stored as text).
EVENT_TYPES = {
    "recommendation_shown",
    "recommendation_explained",
    "recommendation_applied",
    "recommendation_dismissed",
    "chain_generated",
    "chain_applied",
    "graph_applied",
    "export",
    "feedback",
}

# Retention (audit SVC-19): the table was append-only with no bound, while a
# batch render writes up to 200 events per job and payloads reach 32 KB.
# Events older than RETENTION_DAYS are pruned, and past MAX_EVENTS rows the
# oldest go first. Both are overridable for tests/power users via env vars
# (KHENSU_EVENTS_RETENTION_DAYS, KHENSU_EVENTS_MAX_ROWS; 0 disables that rule).
DEFAULT_RETENTION_DAYS = 365
DEFAULT_MAX_EVENTS = 50_000
# Pruning runs on connection setup and then once every this many appends.
PRUNE_EVERY_APPENDS = 500

_lock = threading.Lock()
_appends_since_prune = 0
# Serializes lazy connection setup: two first requests racing through
# _get_conn() must not both open a connection and both run the one-time
# legacy import (the loser's connection would also leak).
_init_lock = threading.Lock()
_conn: sqlite3.Connection | None = None
_legacy_path: str | None = None
_import_pending = False
_next_import_at = 0.0


def _import_legacy(conn: sqlite3.Connection) -> None:
    """Copy rows from the legacy provenance_events.db (ATTACHed as `src`) into
    the unified table. Append-only + id PK, so INSERT OR IGNORE is idempotent."""
    conn.execute(
        "INSERT OR IGNORE INTO provenance_events "
        "(id, session_id, event_type, summary, recommendation_id, payload_json, created_at) "
        "SELECT id, session_id, event_type, summary, recommendation_id, payload_json, created_at "
        "FROM src.provenance_events")


def _get_conn() -> sqlite3.Connection:
    global _conn, _legacy_path, _import_pending, _next_import_at
    conn = _conn
    if conn is not None and not _import_pending:
        return conn
    with _init_lock:
        if _conn is None:
            from khensu_services.common.db_unify import configure_connection, resolve_db
            # Unified store (ARCH-03): provenance events now live in khensu.db,
            # not a separate provenance_events.db, with a one-time backup-first
            # import. An explicit KHENSU_EVENTS_DB override (tests) skips it.
            db_path, legacy = resolve_db("KHENSU_EVENTS_DB", "provenance_events.db")
            conn = sqlite3.connect(db_path, check_same_thread=False)
            try:
                conn.row_factory = sqlite3.Row
                configure_connection(conn)
                conn.executescript(_MIGRATIONS[11])  # provenance_events (ENG-8)
                conn.commit()
            except Exception:
                conn.close()   # never leak a half-initialised handle
                raise
            _conn = conn
            try:
                prune_events(conn=conn)
            except sqlite3.Error:
                pass   # retention is housekeeping; never block the event store
            _legacy_path = legacy
            _import_pending = True
            _next_import_at = 0.0
        # AUDIT-057: a failed legacy import is logged and retried later rather
        # than discarding the connection (see feedback/db.py).
        if _import_pending and time.monotonic() >= _next_import_at:
            from khensu_services.common.db_unify import IMPORT_RETRY_INTERVAL_SEC, try_import_once
            with _lock:
                ok = try_import_once(_conn, "events_from_provenance_events_db",
                                     _legacy_path, _import_legacy)
            _import_pending = not ok
            _next_import_at = time.monotonic() + IMPORT_RETRY_INTERVAL_SEC
    return _conn


def _row_to_dict(row: sqlite3.Row) -> dict:
    try:
        payload = json.loads(row["payload_json"])
    except (ValueError, TypeError):
        payload = {}
    return {
        "id": row["id"],
        "session_id": row["session_id"],
        "event_type": row["event_type"],
        "summary": row["summary"],
        "recommendation_id": row["recommendation_id"],
        "payload": payload,
        "created_at": row["created_at"],
    }


def _env_int(name: str, default: int) -> int:
    try:
        return max(0, int(os.environ.get(name, "").strip() or default))
    except ValueError:
        return default


def prune_events(max_age_days: int | None = None, max_rows: int | None = None,
                 *, conn: sqlite3.Connection | None = None) -> int:
    """Apply the retention policy; returns the number of rows deleted.

    ``max_age_days`` (default ``KHENSU_EVENTS_RETENTION_DAYS`` or 365) drops
    events older than that; ``max_rows`` (default ``KHENSU_EVENTS_MAX_ROWS``
    or 50 000) keeps only the newest rows. 0 disables a rule."""
    if max_age_days is None:
        max_age_days = _env_int("KHENSU_EVENTS_RETENTION_DAYS", DEFAULT_RETENTION_DAYS)
    if max_rows is None:
        max_rows = _env_int("KHENSU_EVENTS_MAX_ROWS", DEFAULT_MAX_EVENTS)
    conn = conn or _get_conn()
    deleted = 0
    with _lock:
        if max_age_days > 0:
            cutoff = (datetime.now(timezone.utc) - timedelta(days=max_age_days)).isoformat()
            # created_at is always datetime.now(timezone.utc).isoformat(), so
            # ISO strings order chronologically.
            deleted += conn.execute(
                "DELETE FROM provenance_events WHERE created_at < ?", (cutoff,)).rowcount
        if max_rows > 0:
            deleted += conn.execute(
                "DELETE FROM provenance_events WHERE rowid IN ("
                "SELECT rowid FROM provenance_events "
                "ORDER BY created_at DESC, rowid DESC LIMIT -1 OFFSET ?)", (max_rows,)).rowcount
        conn.commit()
    return deleted


def append_event(session_id: str, event_type: str, summary: str = "",
                 recommendation_id: str = "", payload: dict | None = None) -> dict:
    global _appends_since_prune
    conn = _get_conn()
    now = datetime.now(timezone.utc).isoformat()
    row_id = str(uuid.uuid4())
    payload_json = json.dumps(payload or {})
    with _lock:
        conn.execute(
            "INSERT INTO provenance_events "
            "(id, session_id, event_type, summary, recommendation_id, payload_json, created_at) "
            "VALUES (?,?,?,?,?,?,?)",
            (row_id, session_id or "", event_type, summary, recommendation_id, payload_json, now),
        )
        conn.commit()
        _appends_since_prune += 1
        due = _appends_since_prune >= PRUNE_EVERY_APPENDS
        if due:
            _appends_since_prune = 0
    if due:
        try:
            prune_events(conn=conn)
        except sqlite3.Error:
            pass
    return {
        "id": row_id, "session_id": session_id or "", "event_type": event_type,
        "summary": summary, "recommendation_id": recommendation_id,
        "payload": payload or {}, "created_at": now,
    }


def list_events(session_id: str | None = None, limit: int = 200) -> list[dict]:
    # Positively bounded (audit DATA-02): SQLite treats any negative LIMIT as
    # "no limit at all", so an un-clamped caller-supplied limit (e.g. -1, or
    # simply a very large number) could return the entire table in one
    # response. Clamped here rather than only at the router so this library
    # function is safe regardless of caller.
    limit = max(1, min(int(limit), 1000))
    conn = _get_conn()
    with _lock:
        if session_id:
            rows = conn.execute(
                "SELECT * FROM provenance_events WHERE session_id = ? "
                "ORDER BY created_at DESC LIMIT ?",
                (session_id, limit),
            ).fetchall()
        else:
            rows = conn.execute(
                "SELECT * FROM provenance_events ORDER BY created_at DESC LIMIT ?",
                (limit,),
            ).fetchall()
    return [_row_to_dict(r) for r in rows]


def delete_events_for_session(session_id: str) -> int:
    """Erases every provenance event for one session (audit DATA-02's
    "Delete session and derived data"). Returns the number of rows deleted."""
    conn = _get_conn()
    with _lock:
        cur = conn.execute("DELETE FROM provenance_events WHERE session_id = ?", (session_id,))
        conn.commit()
        return cur.rowcount


def delete_all_events() -> int:
    """Erases every provenance event (audit DATA-02's "Delete all Khensu
    data"). Irreversible; the caller is responsible for confirming with the
    user first. Returns the number of rows deleted."""
    conn = _get_conn()
    with _lock:
        cur = conn.execute("DELETE FROM provenance_events")
        conn.commit()
        return cur.rowcount
