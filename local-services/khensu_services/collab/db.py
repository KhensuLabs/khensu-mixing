"""khensu.db access for the collaboration tables (migrations 013 and 014) and
the user's genre examples (015, ``analysis/learned/genre_examples.py``).

The desktop app owns the migration chain (storage/schema/NNN_*.sql applied by
SessionManager), but the service may open khensu.db first, so - exactly like
feedback/db.py and events/db.py - it creates the same table shapes itself with
``CREATE TABLE IF NOT EXISTS`` - by running the migrations themselves
(``common/schema_sql.py``, generated from the .sql files; ENG-8).

``KHENSU_COLLAB_DB`` overrides the path (tests isolate a temp DB with it).
"""
from __future__ import annotations

import os
import sqlite3
import threading
from collections.abc import Iterator
from contextlib import contextmanager
from datetime import datetime, timezone

# ENG-8: the tables come from migrations 013-015 themselves, generated into
# common/schema_sql.py by scripts/gen_schema_sql.py - no hand-kept copy.
from khensu_services.common.schema_sql import MIGRATIONS as _MIGRATIONS  # noqa: E402

DDL_013 = _MIGRATIONS[13]  # session_comments
DDL_014 = _MIGRATIONS[14]  # render_versions, render_branches
DDL_015 = _MIGRATIONS[15]  # genre_examples

_lock = threading.RLock()
_conn: sqlite3.Connection | None = None
_conn_path: str | None = None


def _db_path() -> str:
    override = os.environ.get("KHENSU_COLLAB_DB", "").strip()
    if override:
        return override
    from khensu_services.auth import get_token_path
    base = get_token_path().parent
    base.mkdir(parents=True, exist_ok=True)
    return str(base / "khensu.db")


def utc_now() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.%f")[:-3] + "Z"


def _open(path: str) -> sqlite3.Connection:
    from khensu_services.common.db_unify import configure_connection
    conn = sqlite3.connect(path, check_same_thread=False)
    try:
        conn.row_factory = sqlite3.Row
        configure_connection(conn)
        conn.executescript(DDL_013)
        conn.executescript(DDL_014)
        conn.executescript(DDL_015)
        conn.commit()
    except Exception:
        conn.close()
        raise
    return conn


@contextmanager
def connection() -> Iterator[sqlite3.Connection]:
    """The shared handle, serialized by a module lock (sqlite3 connections are
    not safe for concurrent use). Re-opened when KHENSU_COLLAB_DB changes, so
    each test's temp DB is honoured."""
    global _conn, _conn_path
    with _lock:
        path = _db_path()
        if _conn is None or _conn_path != path:
            if _conn is not None:
                try:
                    _conn.close()
                except sqlite3.Error:
                    pass
            _conn = _open(path)
            _conn_path = path
        yield _conn


def close() -> None:
    global _conn, _conn_path
    with _lock:
        if _conn is not None:
            _conn.close()
        _conn = None
        _conn_path = None
