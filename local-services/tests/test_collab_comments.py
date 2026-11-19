"""CE-COL-1 comments + the migration 013/014 DDL parity (collab/db.py)."""
from __future__ import annotations

import re
import sqlite3
from pathlib import Path

import pytest

from khensu_services.collab import db as collab_db

ROOT = Path(__file__).resolve().parents[2]


@pytest.fixture(autouse=True)
def collab_db_path(tmp_path, monkeypatch):
    monkeypatch.setenv("KHENSU_COLLAB_DB", str(tmp_path / "khensu.db"))
    yield
    collab_db.close()


def _normalized(sql: str) -> str:
    lines = [ln.split("--", 1)[0] for ln in sql.splitlines()]
    return re.sub(r"\s+", " ", " ".join(lines)).strip()


def test_service_migrations_are_the_checked_in_files():
    """ENG-8: the service runs the migrations generated from storage/schema
    (common/schema_sql.py); a stale copy fails here."""
    from khensu_services.common.schema_sql import MIGRATIONS
    files = sorted((ROOT / "storage" / "schema").glob("[0-9][0-9][0-9]_*.sql"))
    assert sorted(MIGRATIONS) == [int(f.name[:3]) for f in files]
    for f in files:
        assert MIGRATIONS[int(f.name[:3])] == f.read_text(encoding="utf-8").replace("\r\n", "\n"), f.name


def test_migrations_apply_on_top_of_the_chain(tmp_path):
    """Every storage/schema file in order on one DB, then the service's own
    CREATE IF NOT EXISTS again: no error, the tables exist."""
    conn = sqlite3.connect(str(tmp_path / "chain.db"))
    files = sorted((ROOT / "storage" / "schema").glob("*.sql"))
    numbers = [int(f.name.split("_", 1)[0]) for f in files]
    assert numbers == list(range(1, len(files) + 1))
    for f in files:
        conn.executescript(f.read_text(encoding="utf-8"))
    conn.executescript(collab_db.DDL_013)
    conn.executescript(collab_db.DDL_014)
    conn.executescript(collab_db.DDL_015)
    names = {r[0] for r in conn.execute("SELECT name FROM sqlite_master WHERE type='table'")}
    assert {"session_comments", "render_versions", "render_branches", "genre_examples"} <= names
    conn.close()


def test_comment_crud_over_http(client):
    r = client.post("/comments", json={"session_id": "s1", "text": "Vocal too loud here",
                                       "start_s": 12.5, "author": "Ana"})
    assert r.status_code == 200, r.text
    c = r.json()
    assert c["start_s"] == 12.5 and "end_s" not in c and c["resolved"] is False
    r2 = client.post("/comments", json={"session_id": "s1", "text": "Snare harsh", "start_s": 3.0,
                                        "end_s": 7.0})
    assert r2.json()["end_s"] == 7.0
    client.post("/comments", json={"session_id": "other", "text": "x", "start_s": 1.0})

    listed = client.get("/comments", params={"session_id": "s1"}).json()["comments"]
    assert [x["text"] for x in listed] == ["Snare harsh", "Vocal too loud here"]   # by time

    upd = client.patch(f"/comments/{c['id']}", json={"resolved": True})
    assert upd.status_code == 200 and upd.json()["resolved"] is True
    open_only = client.get("/comments", params={"session_id": "s1", "include_resolved": False}).json()
    assert [x["text"] for x in open_only["comments"]] == ["Snare harsh"]

    upd2 = client.patch(f"/comments/{r2.json()['id']}", json={"clear_end": True, "text": "Snare"})
    assert "end_s" not in upd2.json() and upd2.json()["text"] == "Snare"

    assert client.delete(f"/comments/{c['id']}").json() == {"deleted": True}
    assert client.delete(f"/comments/{c['id']}").status_code == 404
    assert client.patch("/comments/nope", json={"resolved": True}).status_code == 404


@pytest.mark.parametrize("body", [
    {"session_id": "s", "text": "  ", "start_s": 1.0},
    {"session_id": "s", "text": "x", "start_s": -1.0},
    {"session_id": "s", "text": "x", "start_s": 5.0, "end_s": 4.0},
    {"session_id": "", "text": "x", "start_s": 1.0},
    {"session_id": "s", "text": "x" * 5000, "start_s": 1.0},
])
def test_invalid_comment_is_422(client, body):
    assert client.post("/comments", json=body).status_code == 422
