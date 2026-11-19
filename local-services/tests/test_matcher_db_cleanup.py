"""Connection lifetime for the plugin matcher's khensu.db reads (audit AUD-02).

`sqlite3.Connection.__enter__` manages the *transaction*, not the handle, so
`with sqlite3.connect(...)` alone leaks the connection. These tests pin the
observable consequences rather than the mechanism: the handle is closed on
every exit path, and no failure mode escapes as an exception.
"""
from __future__ import annotations

import sqlite3

import pytest

from khensu_services.copilot import matcher


def _make_db(path, *, with_table: bool = True) -> None:
    with sqlite3.connect(str(path)) as conn:
        if with_table:
            conn.execute(
                "CREATE TABLE scanned_plugins (id TEXT, name TEXT, manufacturer TEXT, "
                "category TEXT, format TEXT, file_path TEXT)"
            )
            conn.execute(
                "INSERT INTO scanned_plugins VALUES ('1', 'Pro-Q 4', 'FabFilter', 'EQ', 'VST3', 'C:/pq4.vst3')"
            )
    conn.close()


@pytest.fixture
def track_connections(monkeypatch):
    """Records every Connection handed out so the test can assert closure.

    `matcher.sqlite3` is the stdlib module itself, so this spy is global -- the
    tests call `.clear()` after building their fixture DB so only the
    connections opened by `load_scanned_plugins` are under assertion.
    """
    opened: list[sqlite3.Connection] = []
    real_connect = sqlite3.connect

    def spy(*args, **kwargs):
        conn = real_connect(*args, **kwargs)
        opened.append(conn)
        return conn

    monkeypatch.setattr(matcher.sqlite3, "connect", spy)
    return opened


def _closed(conn: sqlite3.Connection) -> bool:
    try:
        conn.execute("SELECT 1")
    except sqlite3.ProgrammingError:
        return True
    return False


def test_successful_read_closes_the_connection(tmp_path, monkeypatch, track_connections):
    db = tmp_path / "khensu.db"
    _make_db(db)
    monkeypatch.setattr(matcher, "get_khensu_db_path", lambda: db)
    track_connections.clear()  # drop the fixture DB's own connection

    plugins = matcher.load_scanned_plugins()

    assert [p["name"] for p in plugins] == ["Pro-Q 4"]
    assert len(track_connections) == 1
    assert _closed(track_connections[0]), "connection outlived load_scanned_plugins()"


def test_query_failure_still_closes_and_degrades_to_empty(tmp_path, monkeypatch, track_connections):
    """A present-but-schemaless DB is the real-world case: the query raises
    inside the `with`, and the handle must still be released."""
    db = tmp_path / "khensu.db"
    _make_db(db, with_table=False)
    monkeypatch.setattr(matcher, "get_khensu_db_path", lambda: db)
    track_connections.clear()  # drop the fixture DB's own connection

    assert matcher.load_scanned_plugins() == []
    assert len(track_connections) == 1
    assert _closed(track_connections[0]), "connection leaked on the error path"


def test_connect_failure_is_swallowed(tmp_path, monkeypatch):
    """`connect()` itself failing must not raise — this is the path the old
    `except NameError` was silently covering."""
    db = tmp_path / "khensu.db"
    _make_db(db)
    monkeypatch.setattr(matcher, "get_khensu_db_path", lambda: db)

    def boom(*args, **kwargs):
        raise sqlite3.OperationalError("unable to open database file")

    monkeypatch.setattr(matcher.sqlite3, "connect", boom)
    assert matcher.load_scanned_plugins() == []


def test_missing_db_file_returns_empty(tmp_path, monkeypatch):
    monkeypatch.setattr(matcher, "get_khensu_db_path", lambda: tmp_path / "absent.db")
    assert matcher.load_scanned_plugins() == []
