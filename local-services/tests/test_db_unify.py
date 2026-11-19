"""DB unification Phase 2 — import helpers + Python repoint (ARCH-03).

Covers the one-time, backup-first, idempotent import used to fold the legacy
Python stores (service_feedback.db, provenance_events.db) into khensu.db, plus
the feedback/events modules still working against the unified connection.
"""
from __future__ import annotations

import sqlite3

import pytest

from khensu_services.common import db_unify


# --- resolve_db -------------------------------------------------------------

def test_resolve_db_honours_env_override_and_skips_import(monkeypatch):
    monkeypatch.setenv("KHENSU_FEEDBACK_DB", "/tmp/isolated.db")
    db_path, legacy = db_unify.resolve_db("KHENSU_FEEDBACK_DB", "service_feedback.db")
    assert db_path == "/tmp/isolated.db"
    assert legacy is None                      # override -> no legacy import


def test_resolve_db_defaults_to_khensu_db_with_legacy(monkeypatch, tmp_path):
    monkeypatch.delenv("KHENSU_FEEDBACK_DB", raising=False)
    import khensu_services.auth as auth
    monkeypatch.setattr(auth, "get_token_path", lambda: tmp_path / "token.txt")
    db_path, legacy = db_unify.resolve_db("KHENSU_FEEDBACK_DB", "service_feedback.db")
    assert db_path == str(tmp_path / "khensu.db")
    assert legacy == str(tmp_path / "service_feedback.db")


def test_configure_connection_enforces_foreign_keys(tmp_path):
    conn = sqlite3.connect(str(tmp_path / "khensu.db"))
    try:
        db_unify.configure_connection(conn)
        assert conn.execute("PRAGMA foreign_keys").fetchone()[0] == 1
        conn.execute("CREATE TABLE parent (id INTEGER PRIMARY KEY)")
        conn.execute("CREATE TABLE child (parent_id INTEGER REFERENCES parent(id))")
        with pytest.raises(sqlite3.IntegrityError):
            conn.execute("INSERT INTO child (parent_id) VALUES (999)")
    finally:
        conn.close()


# --- import_once ------------------------------------------------------------

def _seed_legacy_feedback(path, rows):
    c = sqlite3.connect(str(path))
    c.execute("CREATE TABLE recommendation_feedback (id TEXT PRIMARY KEY, "
              "recommendation_id TEXT NOT NULL, acted_on INTEGER, feedback_note TEXT, "
              "created_at TEXT, updated_at TEXT)")
    c.executemany("INSERT INTO recommendation_feedback VALUES (?,?,?,?,?,?)", rows)
    c.commit()
    c.close()


def _dest_conn(path):
    c = sqlite3.connect(str(path))
    c.execute("CREATE TABLE recommendation_feedback (id TEXT PRIMARY KEY, "
              "recommendation_id TEXT NOT NULL UNIQUE, acted_on INTEGER, feedback_note TEXT, "
              "created_at TEXT, updated_at TEXT)")
    c.commit()
    return c


def _feedback_importer(conn):
    conn.execute(
        "INSERT INTO recommendation_feedback SELECT * FROM src.recommendation_feedback "
        "WHERE true ON CONFLICT(recommendation_id) DO UPDATE SET "
        "acted_on=excluded.acted_on, feedback_note=excluded.feedback_note, "
        "updated_at=excluded.updated_at WHERE excluded.updated_at > recommendation_feedback.updated_at")


def test_import_once_imports_backs_up_and_renames(tmp_path):
    legacy = tmp_path / "service_feedback.db"
    _seed_legacy_feedback(legacy, [("i1", "r1", 1, "note", "t0", "t0")])
    conn = _dest_conn(tmp_path / "khensu.db")

    status = db_unify.import_once(conn, "fb", str(legacy), _feedback_importer)
    assert status == "imported"
    assert conn.execute("SELECT feedback_note FROM recommendation_feedback "
                        "WHERE recommendation_id='r1'").fetchone()[0] == "note"
    assert (tmp_path / "service_feedback.db.bak").exists()       # backed up
    assert (tmp_path / "service_feedback.db.migrated").exists()  # renamed
    assert not legacy.exists()                                   # original moved


def test_import_once_is_idempotent(tmp_path):
    legacy = tmp_path / "service_feedback.db"
    _seed_legacy_feedback(legacy, [("i1", "r1", 1, "n", "t0", "t0")])
    conn = _dest_conn(tmp_path / "khensu.db")
    assert db_unify.import_once(conn, "fb", str(legacy), _feedback_importer) == "imported"
    # Second call: marker set -> no-op, no error even though legacy is now gone.
    assert db_unify.import_once(conn, "fb", str(legacy), _feedback_importer) == "already-done"
    assert conn.execute("SELECT COUNT(*) FROM recommendation_feedback").fetchone()[0] == 1


def test_import_once_missing_legacy_marks_done(tmp_path):
    conn = _dest_conn(tmp_path / "khensu.db")
    assert db_unify.import_once(conn, "fb", str(tmp_path / "nope.db"), _feedback_importer) == "no-legacy"
    assert db_unify.import_once(conn, "fb", None, _feedback_importer) == "already-done"


def test_import_merge_is_last_write_wins_on_updated_at(tmp_path):
    conn = _dest_conn(tmp_path / "khensu.db")
    # Dest already has r1 at t5; legacy has r1 at t9 (newer) and r2 at t1 (new).
    conn.execute("INSERT INTO recommendation_feedback VALUES "
                 "('d1','r1',0,'old','t0','t5')")
    conn.commit()
    legacy = tmp_path / "service_feedback.db"
    _seed_legacy_feedback(legacy, [("i1", "r1", 1, "new", "t0", "t9"),
                                   ("i2", "r2", 0, "keep", "t0", "t1")])
    db_unify.import_once(conn, "fb", str(legacy), _feedback_importer)
    rows = dict(conn.execute("SELECT recommendation_id, feedback_note FROM "
                             "recommendation_feedback").fetchall())
    assert rows["r1"] == "new"    # newer updated_at won
    assert rows["r2"] == "keep"   # new row imported


def test_import_merge_keeps_newer_dest_over_older_legacy(tmp_path):
    conn = _dest_conn(tmp_path / "khensu.db")
    conn.execute("INSERT INTO recommendation_feedback VALUES ('d1','r1',1,'dest-new','t0','t9')")
    conn.commit()
    legacy = tmp_path / "service_feedback.db"
    _seed_legacy_feedback(legacy, [("i1", "r1", 0, "legacy-old", "t0", "t1")])
    db_unify.import_once(conn, "fb", str(legacy), _feedback_importer)
    note = conn.execute("SELECT feedback_note FROM recommendation_feedback "
                        "WHERE recommendation_id='r1'").fetchone()[0]
    assert note == "dest-new"     # older legacy did NOT overwrite


# --- repointed modules still work (env-override path, no import) ------------

def test_feedback_module_works_against_unified_connection(monkeypatch, tmp_path):
    monkeypatch.setenv("KHENSU_FEEDBACK_DB", str(tmp_path / "u.db"))
    import khensu_services.feedback.db as fdb
    fdb._conn = None
    out = fdb.upsert_feedback("recX", True, "hi")
    assert out["recommendation_id"] == "recX" and out["acted_on"] == 1
    fdb._conn = None


def test_events_module_works_against_unified_connection(monkeypatch, tmp_path):
    monkeypatch.setenv("KHENSU_EVENTS_DB", str(tmp_path / "u.db"))
    import khensu_services.events.db as edb
    edb._conn = None
    edb.append_event("s1", "export", "did a thing")
    assert len(edb.list_events("s1")) == 1
    edb._conn = None


@pytest.mark.parametrize("marker", ["fb", "ev"])
def test_marker_table_created_and_persisted(tmp_path, marker):
    conn = _dest_conn(tmp_path / "khensu.db")
    db_unify.import_once(conn, marker, None, _feedback_importer)
    assert conn.execute("SELECT 1 FROM data_migrations WHERE name=?", (marker,)).fetchone()


def test_feedback_get_conn_imports_legacy_into_khensu_db_end_to_end(monkeypatch, tmp_path):
    # The real (no-override) path: opening the connection unifies onto khensu.db
    # and folds in the legacy service_feedback.db exactly once.
    monkeypatch.delenv("KHENSU_FEEDBACK_DB", raising=False)
    import khensu_services.auth as auth
    monkeypatch.setattr(auth, "get_token_path", lambda: tmp_path / "token.txt")
    _seed_legacy_feedback(tmp_path / "service_feedback.db",
                          [("i1", "rLegacy", 1, "from-legacy", "t0", "t0")])

    import khensu_services.feedback.db as fdb
    fdb._conn = None
    try:
        fdb.upsert_feedback("rNew", False, "fresh")     # triggers _get_conn + import
        conn = fdb._get_conn()
        ids = {r[0] for r in conn.execute(
            "SELECT recommendation_id FROM recommendation_feedback")}
        assert ids == {"rLegacy", "rNew"}               # legacy imported + new write
        # Data landed in the unified khensu.db, and the legacy file was retired.
        assert (tmp_path / "khensu.db").exists()
        assert (tmp_path / "service_feedback.db.migrated").exists()
        assert (tmp_path / "service_feedback.db.bak").exists()
    finally:
        fdb._conn = None
