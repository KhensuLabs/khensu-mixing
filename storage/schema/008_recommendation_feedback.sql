-- Migration 8: Recommendation feedback (DB unification, ARCH-03 / plan
-- 2026-07-23). This table previously lived in TWO separate stores that were
-- never reconciled: feedback.db (C++ FeedbackManager, desktop-UI feedback) and
-- service_feedback.db (Python feedback/db.py, service-endpoint feedback). Both
-- are folded into khensu.db here. Unified schema keeps recommendation_id UNIQUE
-- (the C++ variant's constraint; the Python variant lacked it) so there is one
-- row per recommendation and the Phase-2/3 imports can merge with
-- ON CONFLICT(recommendation_id). Schema-only in Phase 1 - no data is moved yet.

CREATE TABLE IF NOT EXISTS recommendation_feedback (
    id                TEXT PRIMARY KEY,
    recommendation_id TEXT NOT NULL UNIQUE,
    acted_on          INTEGER NOT NULL DEFAULT 0,
    feedback_note     TEXT    NOT NULL DEFAULT '',
    created_at        TEXT    NOT NULL,
    updated_at        TEXT    NOT NULL
);
