-- Migration 9: Parameter-override telemetry (DB unification, ARCH-03 / plan
-- 2026-07-23). Moved from feedback.db (C++ FeedbackManager). Records when the
-- engineer changes a parameter away from the recommended value, feeding
-- adaptive recommendation calibration. Schema is unchanged from the
-- FeedbackManager bootstrap so the Phase-3 import is a straight INSERT..SELECT.
-- Schema-only in Phase 1 - no data moved yet.

CREATE TABLE IF NOT EXISTS parameter_overrides (
    id                TEXT PRIMARY KEY,
    recommendation_id TEXT NOT NULL,
    parameter_name    TEXT NOT NULL,
    recommended_value REAL NOT NULL,
    actual_value      REAL NOT NULL,
    created_at        TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_overrides_rec_id
    ON parameter_overrides(recommendation_id);
