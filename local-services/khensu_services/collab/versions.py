"""Version control for mixes (CE-COL-2): render versions, names, tags, branches.

The desktop render tree (``RenderHistory``) is in-memory; each node is synced
here as a ``render_versions`` row keyed by (session, audio path), so a name, a
tag or a branch survives a restart and a branch can be exported as a bundle.

* A **label** names a version - a restore point the user can return to.
* **Tags** are free-form strings ("client-approved", "radio").
* A **branch** is a named line of work started from a base version (or the
  original). Versions synced while a branch is active carry its name.
* **Restore** is the desktop's job (it makes the audio the current audition
  version); the service only records which version is which.
"""
from __future__ import annotations

import json
import re
import uuid
from typing import Any

from khensu_services.collab.db import connection, utc_now

MAX_LABEL_CHARS = 120
MAX_TAGS = 16
MAX_TAG_CHARS = 40
MAX_VERSIONS_PER_SESSION = 2000
_BRANCH_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9 _.\-]{0,47}$")


class VersionError(ValueError):
    """Invalid version input (mapped to 422 by the router)."""


def _row(r) -> dict[str, Any]:
    try:
        tags = json.loads(r["tags_json"] or "[]")
        if not isinstance(tags, list):
            tags = []
    except ValueError:
        tags = []
    out: dict[str, Any] = {
        "id": r["id"],
        "session_id": r["session_id"],
        "path": r["path"],
        "label": r["label"],
        "tags": [str(t) for t in tags],
        "branch": r["branch"],
        "created_at": r["created_at"],
        "updated_at": r["updated_at"],
    }
    if r["parent_id"]:
        out["parent_id"] = r["parent_id"]
    return out


def clean_branch(name: str) -> str:
    n = (name or "").strip()
    if n and not _BRANCH_RE.match(n):
        raise VersionError("Branch names are 1-48 characters: letters, digits, space, '_', '.', '-'.")
    return n


def _clean_tags(tags: list[str]) -> list[str]:
    out: list[str] = []
    for t in tags:
        s = str(t).strip()[:MAX_TAG_CHARS]
        if s and s not in out:
            out.append(s)
    if len(out) > MAX_TAGS:
        raise VersionError(f"At most {MAX_TAGS} tags per version.")
    return out


def list_versions(session_id: str, branch: str | None = None) -> list[dict[str, Any]]:
    sql = "SELECT * FROM render_versions WHERE session_id = ?"
    args: list[Any] = [session_id]
    if branch is not None:
        sql += " AND branch = ?"
        args.append(branch)
    sql += " ORDER BY created_at, rowid"
    with connection() as conn:
        return [_row(r) for r in conn.execute(sql, args).fetchall()]


def get_version(version_id: str) -> dict[str, Any] | None:
    with connection() as conn:
        r = conn.execute("SELECT * FROM render_versions WHERE id = ?", (version_id,)).fetchone()
    return _row(r) if r else None


def upsert_version(session_id: str, path: str, parent_path: str = "", branch: str = "",
                   label: str | None = None, tags: list[str] | None = None,
                   version_id: str | None = None, created_at: str | None = None) -> dict[str, Any]:
    """Insert the version for (session, path) or return the existing one.

    An existing row keeps its id, branch and creation time (a re-sync of the
    same tree is idempotent); label/tags are only changed when given."""
    if not session_id.strip() or not path.strip():
        raise VersionError("session_id and path are required.")
    branch = clean_branch(branch)
    now = utc_now()
    with connection() as conn:
        existing = conn.execute("SELECT * FROM render_versions WHERE session_id = ? AND path = ?",
                                (session_id, path)).fetchone()
        parent_id = None
        if parent_path:
            p = conn.execute("SELECT id FROM render_versions WHERE session_id = ? AND path = ?",
                             (session_id, parent_path)).fetchone()
            parent_id = p["id"] if p else None
        if existing is None:
            n = conn.execute("SELECT COUNT(*) FROM render_versions WHERE session_id = ?",
                             (session_id,)).fetchone()[0]
            if n >= MAX_VERSIONS_PER_SESSION:
                raise VersionError(f"A session holds at most {MAX_VERSIONS_PER_SESSION} versions.")
            vid = version_id or uuid.uuid4().hex
            conn.execute(
                "INSERT INTO render_versions (id, session_id, path, parent_id, label, tags_json, branch,"
                " created_at, updated_at) VALUES (?,?,?,?,?,?,?,?,?)",
                (vid, session_id, path, parent_id, (label or "").strip()[:MAX_LABEL_CHARS],
                 json.dumps(_clean_tags(tags or [])), branch, created_at or now, now))
        else:
            vid = existing["id"]
            sets, args = ["updated_at = ?"], [now]
            if label is not None:
                sets.append("label = ?")
                args.append(label.strip()[:MAX_LABEL_CHARS])
            if tags is not None:
                sets.append("tags_json = ?")
                args.append(json.dumps(_clean_tags(tags)))
            if parent_id is not None and existing["parent_id"] is None:
                sets.append("parent_id = ?")
                args.append(parent_id)
            conn.execute(f"UPDATE render_versions SET {', '.join(sets)} WHERE id = ?", (*args, vid))
        conn.commit()
    got = get_version(vid)
    assert got is not None
    return got


def update_version(version_id: str, label: str | None = None,
                   tags: list[str] | None = None) -> dict[str, Any] | None:
    current = get_version(version_id)
    if current is None:
        return None
    return upsert_version(current["session_id"], current["path"], label=label, tags=tags)


def delete_version(version_id: str) -> bool:
    """Forget a version (its audio file is untouched). Children are re-parented
    to the deleted version's parent so the lineage stays connected."""
    with connection() as conn:
        r = conn.execute("SELECT parent_id FROM render_versions WHERE id = ?", (version_id,)).fetchone()
        if r is None:
            return False
        conn.execute("UPDATE render_versions SET parent_id = ? WHERE parent_id = ?",
                     (r["parent_id"], version_id))
        conn.execute("UPDATE render_branches SET base_version_id = ? WHERE base_version_id = ?",
                     (r["parent_id"], version_id))
        conn.execute("DELETE FROM render_versions WHERE id = ?", (version_id,))
        conn.commit()
        return True


def list_branches(session_id: str) -> list[dict[str, Any]]:
    with connection() as conn:
        rows = conn.execute("SELECT * FROM render_branches WHERE session_id = ? ORDER BY created_at",
                            (session_id,)).fetchall()
    out = []
    for r in rows:
        b: dict[str, Any] = {"name": r["name"], "created_at": r["created_at"]}
        if r["base_version_id"]:
            b["base_version_id"] = r["base_version_id"]
        out.append(b)
    return out


def create_branch(session_id: str, name: str, base_version_id: str = "") -> dict[str, Any]:
    name = clean_branch(name)
    if not name:
        raise VersionError("A branch needs a name.")
    with connection() as conn:
        if base_version_id:
            r = conn.execute("SELECT session_id FROM render_versions WHERE id = ?",
                             (base_version_id,)).fetchone()
            if r is None or r["session_id"] != session_id:
                raise VersionError("The base version does not belong to this session.")
        if conn.execute("SELECT 1 FROM render_branches WHERE session_id = ? AND name = ?",
                        (session_id, name)).fetchone():
            raise VersionError(f"Branch '{name}' already exists.")
        now = utc_now()
        conn.execute("INSERT INTO render_branches (session_id, name, base_version_id, created_at)"
                     " VALUES (?,?,?,?)", (session_id, name, base_version_id or None, now))
        conn.commit()
    out: dict[str, Any] = {"name": name, "created_at": now}
    if base_version_id:
        out["base_version_id"] = base_version_id
    return out


def branch_lineage(session_id: str, branch: str) -> list[dict[str, Any]]:
    """The versions a branch export carries: every version made on the branch
    plus the ancestor chain of its base (so the export can be read on its own),
    oldest first. Main ('') is every version with an empty branch name."""
    branch = clean_branch(branch)
    versions = list_versions(session_id)
    by_id = {v["id"]: v for v in versions}
    chosen: dict[str, dict[str, Any]] = {v["id"]: v for v in versions if v["branch"] == branch}
    base_id = None
    if branch:
        for b in list_branches(session_id):
            if b["name"] == branch:
                base_id = b.get("base_version_id")
                break
        else:
            raise VersionError(f"No branch named '{branch}'.")
    seeds = [base_id] if base_id else []
    seeds += [v.get("parent_id") for v in chosen.values()]
    guard = 0
    while seeds and guard < 10_000:
        guard += 1
        vid = seeds.pop()
        if not vid or vid in chosen or vid not in by_id:
            continue
        chosen[vid] = by_id[vid]
        seeds.append(by_id[vid].get("parent_id"))
    return sorted(chosen.values(), key=lambda v: (v["created_at"], v["id"]))
