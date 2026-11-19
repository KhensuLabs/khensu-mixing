-- Migration 15: Genres the user confirmed or chose for their own tracks (GEN-1e).
-- One row per track and embedding: the genre probe's feature vector for the
-- track (float32 little-endian), the catalog genre the user settled on, and
-- whether they confirmed the suggestion or chose another genre. The probe
-- blends its prediction with the nearest of these, so it learns the user's
-- music. Rows from a different embedding (embedding_tag) are ignored, not
-- converted. Written by the Python service (collab/db.py creates the same
-- shape itself). Not session-scoped: a deleted session keeps its example;
-- DELETE /genre/examples forgets them all.

CREATE TABLE IF NOT EXISTS genre_examples (
    id             INTEGER PRIMARY KEY AUTOINCREMENT,
    audio_key      TEXT    NOT NULL,           -- file size + mtime + name hash
    embedding_tag  TEXT    NOT NULL,
    genre          TEXT    NOT NULL,           -- catalog genre id
    source         TEXT    NOT NULL,           -- 'confirmed' | 'chosen'
    features       BLOB    NOT NULL,
    created_at     TEXT    NOT NULL,           -- ISO 8601 UTC
    UNIQUE (audio_key, embedding_tag)
);
