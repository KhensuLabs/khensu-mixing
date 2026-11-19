# local-services/khensu_services/feedback/db.py
"""SQLite feedback store. Thread-safe via check_same_thread=False + mutex."""
from __future__ import annotations
import sqlite3
import threading
import time
import uuid
from datetime import datetime, timezone


# ENG-8: the table comes from migration 008 itself (recommendation_id UNIQUE),
# generated into common/schema_sql.py - whichever of the desktop's migration
# chain or this store opens khensu.db first, the shape is the same.
from khensu_services.common.schema_sql import MIGRATIONS as _MIGRATIONS  # noqa: E402

_lock = threading.Lock()
# Serializes lazy connection setup: two first requests racing through
# _get_conn() must not both open a connection and both run the one-time
# legacy import (the loser's connection would also leak).
_init_lock = threading.Lock()
_conn: sqlite3.Connection | None = None
_legacy_path: str | None = None
_import_pending = False
_next_import_at = 0.0


def _import_legacy(conn: sqlite3.Connection) -> None:
    """Merge rows from the legacy service_feedback.db (ATTACHed as `src`) into
    the unified table, last-write-wins on updated_at (the desktop app's own
    recommendation_feedback in feedback.db is merged separately in Phase 3)."""
    conn.execute(
        "INSERT INTO recommendation_feedback "
        "(id, recommendation_id, acted_on, feedback_note, created_at, updated_at) "
        "SELECT id, recommendation_id, acted_on, feedback_note, created_at, updated_at "
        "FROM src.recommendation_feedback WHERE true "
        "ON CONFLICT(recommendation_id) DO UPDATE SET "
        "acted_on=excluded.acted_on, feedback_note=excluded.feedback_note, "
        "updated_at=excluded.updated_at "
        "WHERE excluded.updated_at > recommendation_feedback.updated_at")


def _get_conn() -> sqlite3.Connection:
    global _conn, _legacy_path, _import_pending, _next_import_at
    conn = _conn
    if conn is not None and not _import_pending:
        return conn
    with _init_lock:
        if _conn is None:
            from khensu_services.common.db_unify import configure_connection, resolve_db
            # Unified store (ARCH-03): recommendation feedback now lives in
            # khensu.db, not a separate service_feedback.db, with a one-time
            # backup-first import of any legacy file. An explicit
            # KHENSU_FEEDBACK_DB override (tests) is honoured verbatim and
            # skips the import.
            db_path, legacy = resolve_db("KHENSU_FEEDBACK_DB", "service_feedback.db")
            conn = sqlite3.connect(db_path, check_same_thread=False)
            try:
                conn.row_factory = sqlite3.Row
                configure_connection(conn)
                conn.executescript(_MIGRATIONS[8])  # recommendation_feedback (ENG-8)
                conn.commit()
            except Exception:
                conn.close()   # never leak a half-initialised handle
                raise
            _conn = conn
            _legacy_path = legacy
            _import_pending = True
            _next_import_at = 0.0
        # AUDIT-057: a failed legacy import no longer discards the connection
        # (which 500'd every request and leaked a handle per attempt); it is
        # logged, the store keeps working, and the import retries later.
        if _import_pending and time.monotonic() >= _next_import_at:
            from khensu_services.common.db_unify import IMPORT_RETRY_INTERVAL_SEC, try_import_once
            with _lock:
                ok = try_import_once(_conn, "feedback_from_service_feedback_db",
                                     _legacy_path, _import_legacy)
            _import_pending = not ok
            _next_import_at = time.monotonic() + IMPORT_RETRY_INTERVAL_SEC
    return _conn


def upsert_feedback(recommendation_id: str, acted_on: bool, note: str) -> dict:
    conn = _get_conn()
    now = datetime.now(timezone.utc).isoformat()
    with _lock:
        # One atomic upsert (AUDIT-060): the old SELECT-then-INSERT raced the
        # desktop's writer on the UNIQUE recommendation_id and could fail
        # with an IntegrityError or double-write.
        conn.execute(
            "INSERT INTO recommendation_feedback "
            "(id, recommendation_id, acted_on, feedback_note, created_at, updated_at) "
            "VALUES (?,?,?,?,?,?) "
            "ON CONFLICT(recommendation_id) DO UPDATE SET "
            "acted_on=excluded.acted_on, feedback_note=excluded.feedback_note, "
            "updated_at=excluded.updated_at",
            (str(uuid.uuid4()), recommendation_id, int(acted_on), note, now, now),
        )
        conn.commit()
        row = conn.execute(
            "SELECT * FROM recommendation_feedback WHERE recommendation_id=?",
            (recommendation_id,)
        ).fetchone()
    return dict(row)


def delete_all_feedback() -> int:
    """Erases every row (audit DATA-02's "Delete all Khensu data"). Not
    session-scoped - feedback is keyed by recommendation_id, with no stored
    link back to which session produced it, so there is no per-session
    equivalent here (a real gap this doesn't try to paper over - see
    docs/data-inventory.md). Irreversible; the caller confirms with the user
    first. Returns the number of rows deleted."""
    conn = _get_conn()
    with _lock:
        cur = conn.execute("DELETE FROM recommendation_feedback")
        conn.commit()
        return cur.rowcount


def read_rows(sql: str, params: tuple = ()) -> list[dict]:
    """Run a read-only query on khensu.db and return plain dicts (CE-AI-2's
    taste model reads recommendation_feedback and preference_learning_events
    through this). A table the desktop has not provisioned yet reads as
    empty instead of raising."""
    conn = _get_conn()
    with _lock:
        try:
            return [dict(r) for r in conn.execute(sql, params).fetchall()]
        except sqlite3.OperationalError as exc:
            if "no such table" in str(exc):
                return []
            raise
