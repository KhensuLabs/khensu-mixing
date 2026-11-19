-- Migration 10: Numeric recommendation-calibration preferences (DB unification,
-- ARCH-03 / plan 2026-07-23). Moved from feedback.db (C++ FeedbackManager),
-- where it was confusingly also named `user_preferences` - a NAME COLLISION with
-- khensu.db's existing `user_preferences` (migration 004), which is an unrelated
-- general JSON key/value store (key, value_json, updated_at). This numeric
-- (pref_key, pref_value REAL) store is therefore imported under the distinct
-- name `recommendation_prefs`; khensu.db's JSON user_preferences is untouched.
-- Schema-only in Phase 1 - no data moved yet.

CREATE TABLE IF NOT EXISTS recommendation_prefs (
    pref_key   TEXT PRIMARY KEY,
    pref_value REAL NOT NULL
);
