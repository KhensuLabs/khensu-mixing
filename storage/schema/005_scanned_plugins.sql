-- Migration 5: Cache of scanned VST3/AU plugins for the mixer-chain builder's
-- plugin picker, so a full rescan isn't needed on every app launch.

CREATE TABLE IF NOT EXISTS scanned_plugins (
    id           TEXT PRIMARY KEY,
    name         TEXT NOT NULL,
    manufacturer TEXT NOT NULL,
    category     TEXT NOT NULL,
    format       TEXT NOT NULL,
    file_path    TEXT NOT NULL UNIQUE,
    updated_at   TEXT NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_scanned_plugins_format
    ON scanned_plugins(format);
