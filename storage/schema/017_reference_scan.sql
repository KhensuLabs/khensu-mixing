-- Migration 17: The reference folder scan (REFS-1).
-- The user points the References screen at folders (their music folder, a
-- sample library); the Python service walks them, sorts what it finds into
-- songs and samples, reads their tags and artwork and gives each a catalog
-- genre. Written by the Python service (analysis/reference_scan.py runs this
-- file itself, like 013-015). Not session-scoped.

-- The folders the user chose to scan.
CREATE TABLE IF NOT EXISTS reference_scan_roots (
    path              TEXT    PRIMARY KEY,     -- absolute folder path
    added_at          TEXT    NOT NULL,        -- ISO 8601 UTC
    last_scan_at      TEXT,                    -- ISO 8601 UTC, NULL until scanned
    rescan_on_launch  INTEGER NOT NULL DEFAULT 0
);

-- One row per audio file found under a root.
CREATE TABLE IF NOT EXISTS reference_scan_items (
    id                TEXT    PRIMARY KEY,     -- sha1(normcase(abspath))[:16], the embeddings index id
    root              TEXT    NOT NULL,        -- the scan root it was found under
    path              TEXT    NOT NULL UNIQUE,
    kind              TEXT    NOT NULL,        -- 'song' | 'sample'
    sample_type       TEXT    NOT NULL DEFAULT '',  -- 'loop' | 'one_shot' | '' (songs)
    title             TEXT    NOT NULL DEFAULT '',
    author            TEXT    NOT NULL DEFAULT '',  -- artist (songs) or label/vendor (samples)
    album             TEXT    NOT NULL DEFAULT '',
    pack              TEXT    NOT NULL DEFAULT '',  -- construction pack / sample pack (samples)
    pack_path         TEXT    NOT NULL DEFAULT '',
    artwork_path      TEXT    NOT NULL DEFAULT '',  -- cached thumbnail JPEG, '' when none
    genre             TEXT    NOT NULL DEFAULT '',  -- catalog genre id ('' = unsorted)
    family            TEXT    NOT NULL DEFAULT '',  -- the catalog genre's parent family
    genre_source      TEXT    NOT NULL DEFAULT '',  -- 'tag' | 'folder' | 'pack' | 'probe' | 'ranker' | 'user' | ''
    genre_confidence  REAL    NOT NULL DEFAULT 0.0, -- 0..1
    tag_genre         TEXT    NOT NULL DEFAULT '',  -- the genre text in the file's tags, as written
    bpm               REAL    NOT NULL DEFAULT 0.0,
    musical_key       TEXT    NOT NULL DEFAULT '',
    duration_s        REAL    NOT NULL DEFAULT 0.0,
    size              INTEGER NOT NULL DEFAULT 0,
    mtime_ns          INTEGER NOT NULL DEFAULT 0,
    indexed           INTEGER NOT NULL DEFAULT 0,   -- 1 when the song is in the embeddings index
    error             TEXT    NOT NULL DEFAULT '',
    scanned_at        TEXT    NOT NULL              -- ISO 8601 UTC
);

CREATE INDEX IF NOT EXISTS idx_reference_scan_items_root  ON reference_scan_items(root);
CREATE INDEX IF NOT EXISTS idx_reference_scan_items_genre ON reference_scan_items(genre);
CREATE INDEX IF NOT EXISTS idx_reference_scan_items_kind  ON reference_scan_items(kind);
CREATE INDEX IF NOT EXISTS idx_reference_scan_items_pack  ON reference_scan_items(pack);
