-- Migration 2: Analysis results cache
-- Applied by SessionManager::runMigrations() in C++.
-- Fast-indexed columns for filtering; full AnalysisResult struct in data_json.

CREATE TABLE IF NOT EXISTS analysis_results (
    id              TEXT PRIMARY KEY,
    session_id      TEXT NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,
    file_id         TEXT NOT NULL,
    computed_at     TEXT NOT NULL,
    lufs_integrated REAL,              -- Indexed float for quick loudness queries
    lra             REAL,
    true_peak_dbtp  REAL,
    data_json       TEXT NOT NULL      -- Full AnalysisResult struct serialised via juce::JSON
);

CREATE INDEX IF NOT EXISTS idx_analysis_session
    ON analysis_results(session_id);
