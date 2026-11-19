"""Durable, bounded security event monitoring (2026-07-18 audit SEC-07)."""
import json

import pytest

from khensu_services.common import security_log


@pytest.fixture(autouse=True)
def _isolated_log(tmp_path, monkeypatch):
    """Every test gets its own log file and a cleared in-memory buffer, so
    tests don't see each other's events or touch the real config dir."""
    monkeypatch.setattr(security_log, "_log_path", lambda: tmp_path / "security_events.log")
    security_log.clear_memory_events_for_testing()
    yield
    security_log.clear_memory_events_for_testing()


def test_record_event_appears_in_recent_events():
    security_log.record_event(security_log.KIND_AUTH_FAILURE, "bad token")
    events = security_log.recent_events()
    assert len(events) == 1
    assert events[0]["kind"] == security_log.KIND_AUTH_FAILURE
    assert events[0]["detail"] == "bad token"
    assert "ts" in events[0]


def test_recent_events_returns_newest_first():
    security_log.record_event("a", "first")
    security_log.record_event("a", "second")
    security_log.record_event("a", "third")
    events = security_log.recent_events()
    assert [e["detail"] for e in events] == ["third", "second", "first"]


def test_recent_events_respects_limit():
    for i in range(10):
        security_log.record_event("a", str(i))
    events = security_log.recent_events(limit=3)
    assert len(events) == 3
    assert events[0]["detail"] == "9"


def test_memory_buffer_is_bounded():
    for i in range(security_log._MAX_MEMORY_EVENTS + 50):
        security_log.record_event("a", str(i))
    events = security_log.recent_events(limit=security_log._MAX_MEMORY_EVENTS + 50)
    assert len(events) == security_log._MAX_MEMORY_EVENTS


def test_detail_is_truncated_and_newlines_stripped():
    long_detail = "x" * 500 + "\nsecret-looking-line\r\nmore"
    security_log.record_event("a", long_detail)
    stored = security_log.recent_events()[0]["detail"]
    assert len(stored) <= security_log._MAX_DETAIL_CHARS + 1  # +1 for the ellipsis char
    assert "\n" not in stored
    assert "\r" not in stored


def test_recent_events_excludes_entries_past_the_retention_window(monkeypatch):
    clock = {"now": 1_000_000.0}
    monkeypatch.setattr(security_log.time, "time", lambda: clock["now"])

    security_log.record_event("a", "old")
    clock["now"] += security_log._RETENTION_SEC + 1
    security_log.record_event("a", "new")

    events = security_log.recent_events()
    assert [e["detail"] for e in events] == ["new"]


def test_anomaly_counts_empty_below_threshold():
    for _ in range(security_log.ANOMALY_THRESHOLD - 1):
        security_log.record_event(security_log.KIND_AUTH_FAILURE, "x")
    assert security_log.anomaly_counts() == {}


def test_anomaly_counts_flags_kind_at_or_above_threshold():
    for _ in range(security_log.ANOMALY_THRESHOLD):
        security_log.record_event(security_log.KIND_AUTH_FAILURE, "x")
    assert security_log.anomaly_counts() == {security_log.KIND_AUTH_FAILURE: security_log.ANOMALY_THRESHOLD}


def test_anomaly_counts_ignores_events_outside_the_window(monkeypatch):
    clock = {"now": 1_000_000.0}
    monkeypatch.setattr(security_log.time, "time", lambda: clock["now"])

    for _ in range(security_log.ANOMALY_THRESHOLD):
        security_log.record_event(security_log.KIND_AUTH_FAILURE, "x")

    clock["now"] += security_log.ANOMALY_WINDOW_SEC + 1
    assert security_log.anomaly_counts() == {}


def test_security_events_endpoint_exposes_anomalies(client):
    for _ in range(security_log.ANOMALY_THRESHOLD):
        security_log.record_event(security_log.KIND_MODEL_HASH_MISMATCH, "x")

    resp = client.get("/diagnostics/security_events")
    assert resp.status_code == 200
    body = resp.json()
    assert body["anomalies"].get(security_log.KIND_MODEL_HASH_MISMATCH) == security_log.ANOMALY_THRESHOLD


def test_record_event_never_raises_on_internal_failure(monkeypatch):
    def _boom(*a, **k):
        raise RuntimeError("disk exploded")
    monkeypatch.setattr(security_log, "_append_to_file", _boom)
    security_log.record_event("a", "detail")  # must not raise


def test_events_persist_to_a_bounded_log_file(tmp_path, monkeypatch):
    log_path = tmp_path / "security_events.log"
    monkeypatch.setattr(security_log, "_log_path", lambda: log_path)
    security_log.record_event(security_log.KIND_JOB_ADMISSION_REJECTED, "active=4 max=4")

    assert log_path.exists()
    lines = log_path.read_text(encoding="utf-8").strip().splitlines()
    assert len(lines) == 1
    entry = json.loads(lines[0])
    assert entry["kind"] == security_log.KIND_JOB_ADMISSION_REJECTED


def test_log_file_rotates_past_the_size_cap(tmp_path, monkeypatch):
    log_path = tmp_path / "security_events.log"
    monkeypatch.setattr(security_log, "_log_path", lambda: log_path)
    monkeypatch.setattr(security_log, "_MAX_LOG_FILE_BYTES", 100)

    for i in range(50):
        security_log.record_event("a", f"padding-event-{i}")

    backup = log_path.with_suffix(".log.1")
    assert backup.exists()
    assert log_path.stat().st_size < 100 + 300  # freshly rotated, not unbounded growth


def test_verify_token_records_auth_failure_on_missing_credentials():
    from khensu_services.auth import verify_token
    from fastapi import HTTPException

    with pytest.raises(HTTPException):
        verify_token(credentials=None)

    events = security_log.recent_events()
    assert len(events) == 1
    assert events[0]["kind"] == security_log.KIND_AUTH_FAILURE
    assert "missing" in events[0]["detail"].lower()


def test_verify_token_records_auth_failure_on_mismatched_token(tmp_path, monkeypatch):
    from khensu_services import auth
    from fastapi import HTTPException
    from fastapi.security import HTTPAuthorizationCredentials

    token_path = tmp_path / "token.txt"
    token_path.write_text("the-real-token", encoding="utf-8")
    monkeypatch.setattr(auth, "get_token_path", lambda: token_path)

    creds = HTTPAuthorizationCredentials(scheme="Bearer", credentials="wrong-token")
    with pytest.raises(HTTPException):
        auth.verify_token(credentials=creds)

    events = security_log.recent_events()
    assert len(events) == 1
    assert events[0]["kind"] == security_log.KIND_AUTH_FAILURE
    assert "wrong-token" not in events[0]["detail"]  # never logs the actual token


def test_verify_token_records_nothing_on_success(tmp_path, monkeypatch):
    from khensu_services import auth
    from fastapi.security import HTTPAuthorizationCredentials

    token_path = tmp_path / "token.txt"
    token_path.write_text("the-real-token", encoding="utf-8")
    monkeypatch.setattr(auth, "get_token_path", lambda: token_path)

    creds = HTTPAuthorizationCredentials(scheme="Bearer", credentials="the-real-token")
    auth.verify_token(credentials=creds)  # must not raise

    assert security_log.recent_events() == []


def test_security_events_endpoint_returns_recorded_events(client):
    security_log.record_event(security_log.KIND_JOB_ADMISSION_REJECTED, "active=4 max=4")
    resp = client.get("/diagnostics/security_events")
    assert resp.status_code == 200
    body = resp.json()
    assert body["count"] >= 1
    assert any(e["kind"] == security_log.KIND_JOB_ADMISSION_REJECTED for e in body["events"])


def test_job_admission_rejection_records_a_security_event():
    from khensu_services.common.jobs import JobStore, admit_or_429
    from fastapi import HTTPException

    store = JobStore(max_active=0)
    with pytest.raises(HTTPException):
        admit_or_429(store, "job-1", {"status": "pending"})

    events = security_log.recent_events()
    assert len(events) == 1
    assert events[0]["kind"] == security_log.KIND_JOB_ADMISSION_REJECTED


def test_demucs_downloader_start_records_gate_block_when_disabled(tmp_path, monkeypatch):
    from khensu_services.copilot.downloader import ModelDownloader

    ModelDownloader._instance = None
    monkeypatch.setattr("khensu_services.auth.get_token_path", lambda: tmp_path / "token.txt")
    monkeypatch.setattr("khensu_services.common.online.online_allowed", lambda feature: False)
    try:
        d = ModelDownloader()
        d.start()
    finally:
        ModelDownloader._instance = None

    events = security_log.recent_events()
    assert len(events) == 1
    assert events[0]["kind"] == security_log.KIND_ONLINE_GATE_BLOCKED
    assert "downloader" in events[0]["detail"]


def test_demucs_separator_records_gate_block_when_disabled(monkeypatch):
    from khensu_services.analysis.separator import separate_stems_demucs

    monkeypatch.setattr("khensu_services.common.online.online_allowed", lambda feature: False)

    # Cached-only semantics (SEC-02) mean this only gate-blocks when the
    # checkpoint isn't already verified-cached — force that branch
    # deterministically instead of depending on this machine's real cache.
    class _FakeDownloader:
        def load_config(self):
            pass

        def is_installed(self):
            return False

    monkeypatch.setattr(
        "khensu_services.copilot.downloader.ModelDownloader", lambda: _FakeDownloader()
    )

    result = separate_stems_demucs("nonexistent.wav", "out")

    assert result is None
    events = security_log.recent_events()
    assert len(events) == 1
    assert events[0]["kind"] == security_log.KIND_ONLINE_GATE_BLOCKED
    assert "separation" in events[0]["detail"]


def test_watermark_precache_endpoint_records_gate_block_when_disabled(client, monkeypatch):
    monkeypatch.setattr("khensu_services.common.online.online_allowed", lambda feature: False)
    resp = client.post("/analyze/models/watermark/precache")

    assert resp.status_code == 403
    events = security_log.recent_events()
    assert len(events) == 1
    assert events[0]["kind"] == security_log.KIND_ONLINE_GATE_BLOCKED
    assert "watermark" in events[0]["detail"]


def test_lyric_analyzer_get_model_records_gate_block_when_disabled(monkeypatch):
    from khensu_services.analysis.lyric_analyzer import _get_model, ASRNotAvailableError

    import sys
    import types

    class _UncachedWhisper:
        def __init__(self, *a, local_files_only=False, **k):
            if local_files_only:
                raise RuntimeError("simulated cache miss")
            raise AssertionError("network load attempted with downloads disabled")

    monkeypatch.setitem(sys.modules, "faster_whisper",
                        types.SimpleNamespace(WhisperModel=_UncachedWhisper))
    monkeypatch.setattr("khensu_services.common.online.online_allowed", lambda feature: False)
    with pytest.raises(ASRNotAvailableError):
        _get_model("tiny")

    events = security_log.recent_events()
    assert len(events) == 1
    assert events[0]["kind"] == security_log.KIND_ONLINE_GATE_BLOCKED
    assert "tiny" in events[0]["detail"]
