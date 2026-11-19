"""Timestamped session comments (CE-COL-1).

A comment is pinned to a point (``start_s``) or a range (``start_s``..
``end_s``) of a session's audio, has an author and can be resolved. Stored in
``session_comments`` (migration 013).
"""
from __future__ import annotations

import math
import uuid
from typing import Any

from khensu_services.collab.db import connection, utc_now

MAX_TEXT_CHARS = 4000
MAX_AUTHOR_CHARS = 80
MAX_COMMENTS_PER_SESSION = 5000


class CommentError(ValueError):
    """Invalid comment input (mapped to 422 by the router)."""


def _row(r) -> dict[str, Any]:
    out: dict[str, Any] = {
        "id": r["id"],
        "session_id": r["session_id"],
        "author": r["author"],
        "start_s": float(r["start_s"]),
        "text": r["text"],
        "resolved": bool(r["resolved"]),
        "created_at": r["created_at"],
        "updated_at": r["updated_at"],
    }
    if r["end_s"] is not None:
        out["end_s"] = float(r["end_s"])
    return out


def _check_times(start_s: float, end_s: float | None) -> None:
    if not math.isfinite(start_s) or start_s < 0:
        raise CommentError("start_s must be a finite time >= 0 seconds.")
    if end_s is not None:
        if not math.isfinite(end_s) or end_s < start_s:
            raise CommentError("end_s must be a finite time >= start_s.")


def _clean_text(text: str) -> str:
    t = (text or "").strip()
    if not t:
        raise CommentError("Comment text is empty.")
    if len(t) > MAX_TEXT_CHARS:
        raise CommentError(f"Comment text is longer than {MAX_TEXT_CHARS} characters.")
    return t


def _clean_author(author: str) -> str:
    a = (author or "").strip()
    return a[:MAX_AUTHOR_CHARS]


def list_comments(session_id: str, include_resolved: bool = True) -> list[dict[str, Any]]:
    sql = "SELECT * FROM session_comments WHERE session_id = ?"
    if not include_resolved:
        sql += " AND resolved = 0"
    sql += " ORDER BY start_s, created_at"
    with connection() as conn:
        return [_row(r) for r in conn.execute(sql, (session_id,)).fetchall()]


def get_comment(comment_id: str) -> dict[str, Any] | None:
    with connection() as conn:
        r = conn.execute("SELECT * FROM session_comments WHERE id = ?", (comment_id,)).fetchone()
    return _row(r) if r else None


def add_comment(session_id: str, text: str, start_s: float, end_s: float | None = None,
                author: str = "", resolved: bool = False, comment_id: str | None = None,
                created_at: str | None = None) -> dict[str, Any]:
    if not session_id.strip():
        raise CommentError("session_id is required.")
    _check_times(float(start_s), None if end_s is None else float(end_s))
    body = _clean_text(text)
    now = utc_now()
    cid = comment_id or uuid.uuid4().hex
    with connection() as conn:
        n = conn.execute("SELECT COUNT(*) FROM session_comments WHERE session_id = ?",
                         (session_id,)).fetchone()[0]
        if n >= MAX_COMMENTS_PER_SESSION:
            raise CommentError(f"A session holds at most {MAX_COMMENTS_PER_SESSION} comments.")
        conn.execute(
            "INSERT INTO session_comments (id, session_id, author, start_s, end_s, text, resolved,"
            " created_at, updated_at) VALUES (?,?,?,?,?,?,?,?,?)",
            (cid, session_id, _clean_author(author), float(start_s),
             None if end_s is None else float(end_s), body, 1 if resolved else 0,
             created_at or now, now))
        conn.commit()
    got = get_comment(cid)
    assert got is not None
    return got


def update_comment(comment_id: str, *, text: str | None = None, resolved: bool | None = None,
                   start_s: float | None = None, end_s: float | None = None,
                   clear_end: bool = False) -> dict[str, Any] | None:
    current = get_comment(comment_id)
    if current is None:
        return None
    new_start = current["start_s"] if start_s is None else float(start_s)
    new_end = None if clear_end else (end_s if end_s is not None else current.get("end_s"))
    _check_times(new_start, None if new_end is None else float(new_end))
    new_text = current["text"] if text is None else _clean_text(text)
    new_resolved = current["resolved"] if resolved is None else bool(resolved)
    with connection() as conn:
        conn.execute(
            "UPDATE session_comments SET text = ?, resolved = ?, start_s = ?, end_s = ?, updated_at = ?"
            " WHERE id = ?",
            (new_text, 1 if new_resolved else 0, new_start,
             None if new_end is None else float(new_end), utc_now(), comment_id))
        conn.commit()
    return get_comment(comment_id)


def delete_comment(comment_id: str) -> bool:
    with connection() as conn:
        cur = conn.execute("DELETE FROM session_comments WHERE id = ?", (comment_id,))
        conn.commit()
        return cur.rowcount > 0
