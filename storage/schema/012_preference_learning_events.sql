-- Migration 12: Evidence log behind the learned taste offsets.
-- UserPreferenceModel keeps four global offsets (pref_lufs_offset,
-- pref_lra_offset, pref_stereo_width_offset, pref_low_end_weight_offset) in
-- recommendation_prefs, but stored only the current value - not why it moved.
-- One row per accept/reject or A/B pick that changed an offset, so the
-- "What Khensu has learned" view can say "from 4 rejects, last on <date>" and
-- a per-preference reset can clear its own evidence. FeedbackManager creates
-- the same shape itself when it opens khensu.db first.

CREATE TABLE IF NOT EXISTS preference_learning_events (
    id                INTEGER PRIMARY KEY AUTOINCREMENT,
    pref_key          TEXT    NOT NULL,
    source            TEXT    NOT NULL,             -- 'recommendation' | 'ab_pick'
    recommendation_id TEXT    NOT NULL DEFAULT '',
    accepted          INTEGER NOT NULL,             -- 1 accepted / picked processed, 0 rejected / picked original
    value_before      REAL    NOT NULL,
    value_after       REAL    NOT NULL,
    created_at        TEXT    NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_pref_learning_key
    ON preference_learning_events(pref_key);
