# local-services/khensu_services/common/db_unify.py
"""DB-unification helpers (ARCH-03, plan docs/archive/2026-07-23-db-unification-plan.md).

Phase 2 repoints the Python-owned stores (recommendation feedback, provenance
events) onto the single khensu.db and does a one-time, backup-first, idempotent
import of the legacy per-store files. A `data_migrations` marker table guards
each import so it runs exactly once and is safe to re-enter after a crash — the
old file is backed up (`*.bak`) and renamed (`*.migrated`) but never deleted, so
the move is always reversible.
"""
from __future__ import annotations

import logging
import os
import shutil
import sqlite3
from datetime import datetime, timezone
from typing import Callable

_log = logging.getLogger(__name__)

# SQLite keeps committed-but-uncheckpointed pages of a WAL-mode database in
# `<db>-wal` (and an index in `<db>-shm`). A backup or rename that moves only
# the main file silently drops those rows (AUDIT-059), so every file-level step
# of the import carries the sidecars along.
_SIDECARS = ("-wal", "-shm", "-journal")

# How long a store waits before retrying a legacy import that failed
# (AUDIT-057): the store stays usable on the unified DB meanwhile.
IMPORT_RETRY_INTERVAL_SEC = 60.0


def resolve_db(env_var: str, legacy_name: str) -> tuple[str, str | None]:
    """Return (db_path, legacy_path_or_None).

    An explicit `env_var` override (used by tests to isolate a temp DB) is
    honoured verbatim and disables the legacy import. Otherwise the store lives
    in the unified khensu.db next to the auth token, and the legacy per-store
    file (e.g. service_feedback.db) is returned as the one-time import source."""
    override = os.environ.get(env_var, "").strip()
    if override:
        return override, None
    from khensu_services.auth import get_token_path
    base = get_token_path().parent
    base.mkdir(parents=True, exist_ok=True)
    return str(base / "khensu.db"), str(base / legacy_name)


def configure_connection(conn: sqlite3.Connection) -> None:
    """Match khensu.db's cross-process settings so the service can share the
    file with the desktop app (both write it now) without spurious SQLITE_BUSY:
    WAL for concurrent readers + a serialized writer, a busy timeout, and
    per-connection foreign-key enforcement."""
    # SQLite does not persist this setting in the database file. Every Python
    # handle must enable it before reads/writes so service-owned tables cannot
    # bypass relationships enforced by the desktop connection.
    conn.execute("PRAGMA foreign_keys=ON")
    conn.execute("PRAGMA busy_timeout=5000")
    conn.execute("PRAGMA journal_mode=WAL")


def _ensure_marker_table(conn: sqlite3.Connection) -> None:
    conn.execute(
        "CREATE TABLE IF NOT EXISTS data_migrations ("
        "name TEXT PRIMARY KEY, applied_at TEXT NOT NULL)")


def import_once(conn: sqlite3.Connection, marker: str, legacy_path: str | None,
                importer: Callable[[sqlite3.Connection], None]) -> str:
    """Run `importer(conn)` exactly once, guarded by the `data_migrations`
    marker. Returns "already-done" | "no-legacy" | "imported".

    `importer` receives `conn` with the legacy file ATTACHed as `src` and must
    copy rows idempotently (INSERT OR IGNORE / ON CONFLICT). The importer's
    writes and the marker row commit together, so a crash before commit leaves
    the marker unset and the (idempotent) import simply re-runs. The legacy file
    is copied to `*.bak` first and renamed `*.migrated` on success — never
    deleted, so the operation is reversible."""
    _ensure_marker_table(conn)
    conn.commit()
    if conn.execute("SELECT 1 FROM data_migrations WHERE name=?", (marker,)).fetchone():
        return "already-done"

    now = datetime.now(timezone.utc).isoformat()
    if not legacy_path or not os.path.isfile(legacy_path):
        conn.execute("INSERT OR IGNORE INTO data_migrations (name, applied_at) VALUES (?,?)",
                     (marker, now))
        conn.commit()
        return "no-legacy"

    _backup_with_sidecars(legacy_path, legacy_path + ".bak")

    conn.commit()   # no open transaction before ATTACH (SQLite requirement)
    conn.execute("ATTACH DATABASE ? AS src", (legacy_path,))
    try:
        importer(conn)
        conn.execute("INSERT OR IGNORE INTO data_migrations (name, applied_at) VALUES (?,?)",
                     (marker, now))
        conn.commit()
    except Exception:
        conn.rollback()
        _detach_quietly(conn)
        raise
    # Fold any uncheckpointed WAL pages into the legacy main file before it is
    # retired, so the *.migrated copy is complete on its own.
    try:
        conn.execute("PRAGMA src.wal_checkpoint(TRUNCATE)")
    except sqlite3.Error:
        pass
    _detach_quietly(conn)

    try:
        _move_with_sidecars(legacy_path, legacy_path + ".migrated")
    except OSError:
        pass   # non-fatal: the import already committed
    return "imported"


def try_import_once(conn: sqlite3.Connection, marker: str, legacy_path: str | None,
                    importer: Callable[[sqlite3.Connection], None]) -> bool:
    """``import_once`` that never raises (AUDIT-057). A failed import is logged
    and reported as False so the caller keeps its (fully usable) connection and
    retries later, instead of discarding it and failing every request."""
    try:
        import_once(conn, marker, legacy_path, importer)
        return True
    except Exception:
        _log.exception("Legacy import %r from %s failed; will retry in %.0fs",
                       marker, legacy_path, IMPORT_RETRY_INTERVAL_SEC)
        try:
            conn.rollback()
        except sqlite3.Error:
            pass
        return False


def _detach_quietly(conn: sqlite3.Connection) -> None:
    try:
        conn.execute("DETACH DATABASE src")
    except sqlite3.Error:
        pass


def _backup_with_sidecars(src: str, bak: str) -> None:
    """Copy ``src`` (and any sidecar) to ``bak``, verifying each copy.

    Backup-first means the import must not proceed without a complete backup:
    a copy failure raises, and a backup left truncated by an earlier crash is
    redone rather than trusted."""
    for suffix in ("",) + _SIDECARS:
        source = src + suffix
        target = bak + suffix
        if not os.path.exists(source):
            continue
        if os.path.exists(target) and os.path.getsize(target) == os.path.getsize(source):
            continue
        shutil.copy2(source, target)
        if os.path.getsize(target) != os.path.getsize(source):
            raise OSError(f"Backup of {source} is incomplete")


def _move_with_sidecars(src: str, dst: str) -> None:
    os.replace(src, dst)
    for suffix in _SIDECARS:
        if os.path.exists(src + suffix):
            try:
                os.replace(src + suffix, dst + suffix)
            except OSError:
                pass
