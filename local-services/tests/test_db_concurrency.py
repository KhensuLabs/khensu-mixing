"""Concurrent-writer stress test for the unified khensu.db (ARCH-03 Phase 4).

Unification's main new risk: the desktop app and the Python service now write
ONE file instead of four separate ones. This asserts the WAL + busy_timeout
settings db_unify.configure_connection() applies actually let two independent
connections (standing in for the two processes) interleave writes without
SQLITE_BUSY / "database is locked" failures.
"""
from __future__ import annotations

import sqlite3
import threading

from khensu_services.common import db_unify


def _conn(path: str) -> sqlite3.Connection:
    # This test hammers writes ~100x faster than production ever does, so it
    # needs far more busy-wait headroom than production's 5 s to stay reliable
    # under CI load — the guarantee under test (WAL lets concurrent writers
    # serialize without data loss) is the same, only the artificial write rate
    # differs. `timeout` is Python sqlite3's busy handler; the PRAGMA matches it.
    c = sqlite3.connect(path, check_same_thread=False, timeout=60.0)
    db_unify.configure_connection(c)   # WAL (as in production)
    c.execute("PRAGMA busy_timeout=60000")
    return c


def test_two_connections_write_one_khensu_db_without_locking(tmp_path):
    db = str(tmp_path / "khensu.db")
    setup = _conn(db)
    setup.execute("CREATE TABLE t (id INTEGER PRIMARY KEY AUTOINCREMENT, who TEXT, v INTEGER)")
    setup.commit()

    a, b = _conn(db), _conn(db)
    errors: list[str] = []
    n = 80

    def hammer(conn: sqlite3.Connection, who: str) -> None:
        for i in range(n):
            try:
                conn.execute("INSERT INTO t (who, v) VALUES (?, ?)", (who, i))
                conn.commit()
            except Exception as e:      # noqa: BLE001 - any lock error fails the test
                errors.append(f"{who}: {e}")

    threads = [threading.Thread(target=hammer, args=(a, "app")),
               threading.Thread(target=hammer, args=(b, "service"))]
    for t in threads:
        t.start()
    for t in threads:
        t.join()

    assert not errors, f"concurrent writers hit lock errors: {errors[:5]}"
    total = setup.execute("SELECT COUNT(*) FROM t").fetchone()[0]
    assert total == 2 * n               # every write from both connections landed
    for who in ("app", "service"):
        assert setup.execute("SELECT COUNT(*) FROM t WHERE who=?", (who,)).fetchone()[0] == n


def test_concurrent_writes_to_different_tables_interleave(tmp_path):
    # The realistic shape: the app writes recommendation_feedback while the
    # service writes provenance_events, same file.
    db = str(tmp_path / "khensu.db")
    setup = _conn(db)
    setup.execute("CREATE TABLE recommendation_feedback (id TEXT PRIMARY KEY, "
                  "recommendation_id TEXT UNIQUE, acted_on INTEGER, feedback_note TEXT, "
                  "created_at TEXT, updated_at TEXT)")
    setup.execute("CREATE TABLE provenance_events (id TEXT PRIMARY KEY, session_id TEXT, "
                  "event_type TEXT, summary TEXT, recommendation_id TEXT, payload_json TEXT, created_at TEXT)")
    setup.commit()

    app, svc = _conn(db), _conn(db)
    errors: list[str] = []

    def write_feedback() -> None:
        for i in range(80):
            try:
                app.execute("INSERT INTO recommendation_feedback VALUES (?,?,?,?,?,?)",
                            (f"id{i}", f"rec{i}", 1, "n", "t", "t"))
                app.commit()
            except Exception as e:      # noqa: BLE001
                errors.append(f"feedback: {e}")

    def write_events() -> None:
        for i in range(80):
            try:
                svc.execute("INSERT INTO provenance_events VALUES (?,?,?,?,?,?,?)",
                            (f"ev{i}", "s", "export", "", "", "{}", "t"))
                svc.commit()
            except Exception as e:      # noqa: BLE001
                errors.append(f"events: {e}")

    threads = [threading.Thread(target=write_feedback), threading.Thread(target=write_events)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()

    assert not errors, f"cross-table concurrent writers hit lock errors: {errors[:5]}"
    assert setup.execute("SELECT COUNT(*) FROM recommendation_feedback").fetchone()[0] == 80
    assert setup.execute("SELECT COUNT(*) FROM provenance_events").fetchone()[0] == 80
