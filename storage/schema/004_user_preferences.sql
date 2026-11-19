-- Migration 4: Key/value store for user preferences and system brain state.
-- Keys are namespaced strings, e.g. "ui.theme", "brain.genre_weights.trap".

CREATE TABLE IF NOT EXISTS user_preferences (
    key        TEXT PRIMARY KEY,
    value_json TEXT NOT NULL,
    updated_at TEXT NOT NULL
);
