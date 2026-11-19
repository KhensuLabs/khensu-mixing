-- Migration 11: Provenance / decision-timeline events (DB unification, ARCH-03 /
-- plan 2026-07-23). Moved from provenance_events.db (Python events/db.py), an
-- append-only log of what the recommendation engine showed/did per session.
-- Schema + index are unchanged from the Python bootstrap so the Phase-2 import
-- is a straight INSERT..SELECT. Schema-only in Phase 1 - no data moved yet.

CREATE TABLE IF NOT EXISTS provenance_events (
    id                TEXT PRIMARY KEY,
    session_id        TEXT NOT NULL DEFAULT '',
    event_type        TEXT NOT NULL,
    summary           TEXT NOT NULL DEFAULT '',
    recommendation_id TEXT NOT NULL DEFAULT '',
    payload_json      TEXT NOT NULL DEFAULT '{}',
    created_at        TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_events_session
    ON provenance_events (session_id, created_at);
