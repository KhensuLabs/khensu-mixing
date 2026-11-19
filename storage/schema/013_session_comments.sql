-- Migration 13: Timestamped session comments (CE-COL-1, shared sessions).
-- One row per comment pinned to a time (start_s) or a range (start_s..end_s)
-- of a session's audio. Written by the Python service (collab/db.py creates
-- the same shape itself when it opens khensu.db before the desktop app has
-- migrated it) and read by the desktop comments lane.
--
-- No foreign key to sessions(id): a comment can arrive from an imported
-- .khensu bundle, and the service may create this table before the desktop
-- has created `sessions`. SessionManager::deleteSession deletes a session's
-- comments explicitly instead.

CREATE TABLE IF NOT EXISTS session_comments (
    id          TEXT    PRIMARY KEY,           -- uuid4 hex
    session_id  TEXT    NOT NULL,
    author      TEXT    NOT NULL DEFAULT '',
    start_s     REAL    NOT NULL,              -- seconds from the start of the audio
    end_s       REAL,                          -- NULL = a point comment
    text        TEXT    NOT NULL,
    resolved    INTEGER NOT NULL DEFAULT 0,    -- 0 open / 1 resolved
    created_at  TEXT    NOT NULL,              -- ISO 8601 UTC
    updated_at  TEXT    NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_session_comments_session
    ON session_comments(session_id, start_s);
