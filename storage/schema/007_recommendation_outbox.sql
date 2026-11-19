-- Migration 7: Transactional outbox (audit ARCH-03): recommendation-resolution
-- mirroring to the Python feedback/provenance stores was a fire-and-forget
-- detached HTTP call with no durable record that it needed to happen - a
-- service outage or app close mid-flight silently dropped the mirror
-- forever, leaving khensu.db's local learning state ahead of what
-- reports/the decision timeline saw. A row here is written in the SAME local
-- commit as the action it describes, so "did we mean to send this" survives
-- a crash even if "did we succeed in sending it" doesn't yet.

CREATE TABLE IF NOT EXISTS recommendation_outbox (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    idempotency_key TEXT    NOT NULL UNIQUE,
    kind            TEXT    NOT NULL,
    payload_json    TEXT    NOT NULL,
    status          TEXT    NOT NULL DEFAULT 'pending'
                    CHECK(status IN ('pending','delivered','failed')),
    attempts        INTEGER NOT NULL DEFAULT 0,
    created_at      TEXT    NOT NULL,
    last_attempt_at TEXT,
    last_error      TEXT
);
CREATE INDEX IF NOT EXISTS idx_recommendation_outbox_status
    ON recommendation_outbox(status);
