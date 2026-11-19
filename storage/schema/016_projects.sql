-- Migration 16: Project and album groups (FEAT-6).
-- A project is an ordered set of sessions, one per track: an album, an EP,
-- or the files of one multi-file drop. A session belongs to at most one
-- project; its position is the track order (the album sequence). Deleting a
-- session removes its track row (SessionManager::deleteSession); deleting a
-- project keeps its sessions.

CREATE TABLE IF NOT EXISTS projects (
    id          TEXT PRIMARY KEY,              -- UUID
    name        TEXT NOT NULL,
    kind        TEXT NOT NULL DEFAULT 'album', -- 'album' | 'ep' | 'project'
    created_at  TEXT NOT NULL,                 -- ISO 8601 UTC
    updated_at  TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS project_tracks (
    project_id  TEXT    NOT NULL REFERENCES projects(id) ON DELETE CASCADE,
    session_id  TEXT    NOT NULL UNIQUE,       -- one project per session
    position    INTEGER NOT NULL,              -- 0-based track order
    PRIMARY KEY (project_id, session_id)
);
CREATE INDEX IF NOT EXISTS idx_project_tracks_order
    ON project_tracks(project_id, position);
