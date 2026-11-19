-- Migration 14: Version control for mixes (CE-COL-2).
-- The desktop render tree (RenderHistory) lived only in memory, so names,
-- tags and branches had nowhere to persist. render_versions is the durable
-- copy of each render-tree node: its audio path, its parent (the version it
-- was rendered from; NULL = the original source), a user label (a named
-- restore point), free-form tags, and the branch it was made on ('' = main).
-- render_branches names a line of work and the version it was branched from.
-- Written by the Python service (collab/db.py creates the same shape itself).
-- No foreign key to sessions(id), for the same reason as session_comments;
-- SessionManager::deleteSession deletes both explicitly.

CREATE TABLE IF NOT EXISTS render_versions (
    id          TEXT    PRIMARY KEY,           -- uuid4 hex
    session_id  TEXT    NOT NULL,
    path        TEXT    NOT NULL,              -- rendered audio file
    parent_id   TEXT,                          -- render_versions.id, NULL = from the original
    label       TEXT    NOT NULL DEFAULT '',   -- non-empty = a named restore point
    tags_json   TEXT    NOT NULL DEFAULT '[]', -- JSON array of strings
    branch      TEXT    NOT NULL DEFAULT '',   -- '' = main
    created_at  TEXT    NOT NULL,
    updated_at  TEXT    NOT NULL,
    UNIQUE (session_id, path)
);
CREATE INDEX IF NOT EXISTS idx_render_versions_session
    ON render_versions(session_id, created_at);

CREATE TABLE IF NOT EXISTS render_branches (
    session_id      TEXT NOT NULL,
    name            TEXT NOT NULL,
    base_version_id TEXT,                      -- NULL = branched from the original
    created_at      TEXT NOT NULL,
    PRIMARY KEY (session_id, name)
);
