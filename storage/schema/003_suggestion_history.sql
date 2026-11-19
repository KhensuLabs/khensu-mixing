-- Migration 3: User action history on recommendations
-- Enables the System Brain to learn from accepted/rejected suggestions over time (SP-11).

CREATE TABLE IF NOT EXISTS suggestion_history (
    id                TEXT PRIMARY KEY,
    session_id        TEXT NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,
    recommendation_id TEXT NOT NULL,
    action            TEXT NOT NULL CHECK(action IN ('accepted','rejected','ignored')),
    acted_at          TEXT NOT NULL,
    data_json         TEXT NOT NULL   -- Full Recommendation snapshot at the time of action
);

CREATE INDEX IF NOT EXISTS idx_suggestions_session
    ON suggestion_history(session_id);
