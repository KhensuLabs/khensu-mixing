"""Service security / persistence / infra fixes from the 2026-10-15 audit
(AUDIT-020, 021, 023, 057, 059, 060, 063, 073, 074, 075, 076, 077, 087)."""
from __future__ import annotations

import asyncio
import datetime
import hashlib
import hmac
import json
import sqlite3
import threading

import numpy as np
import pytest
import soundfile as sf
from fastapi import HTTPException
from fastapi.testclient import TestClient

from khensu_services import auth
from khensu_services.auth import verify_token
from khensu_services.server import _MAX_REQUEST_BODY_BYTES, app

client = TestClient(app)


@pytest.fixture
def token_file(tmp_path, monkeypatch):
    path = tmp_path / "token.txt"
    path.write_text("s3cret-token-value\n", encoding="utf-8")
    monkeypatch.setattr(auth, "get_token_path", lambda: path)
    return path


@pytest.fixture
def real_auth():
    """Undo the suite-wide verify_token bypass for this test."""
    app.dependency_overrides.pop(verify_token, None)
    yield


# --------------------------------------------------------------- AUDIT-020
def test_health_nonce_returns_hmac_proof(token_file):
    nonce = "00112233445566778899aabbccddeeff"
    body = client.get(f"/health?nonce={nonce}").json()
    expected = hmac.new(b"s3cret-token-value", nonce.encode(), hashlib.sha256).hexdigest()
    assert body["proof"] == expected
    assert body["proof"] == body["proof"].lower()
    assert body["status"] == "ok"


def test_health_without_nonce_has_no_proof(token_file):
    assert "proof" not in client.get("/health").json()


@pytest.mark.parametrize("nonce", ["abc", "zz" * 10, "0" * 129])
def test_health_rejects_malformed_nonce(token_file, nonce):
    assert client.get(f"/health?nonce={nonce}").status_code == 422


def test_health_stays_unauthenticated(token_file, real_auth):
    assert client.get("/health?nonce=" + "ab" * 8).status_code == 200


# --------------------------------------------------------------- AUDIT-021
def _run_asgi(method: str, path: str, headers: list[tuple[bytes, bytes]],
              chunks: list[bytes]):
    """Drive the app at the ASGI level so we can see exactly how much body it
    pulled. Returns (status, json_body, number_of_receive_calls)."""
    calls = {"receive": 0}
    sent: list[dict] = []
    queue = list(chunks)

    async def receive():
        calls["receive"] += 1
        if queue:
            chunk = queue.pop(0)
            return {"type": "http.request", "body": chunk, "more_body": bool(queue)}
        return {"type": "http.request", "body": b"", "more_body": False}

    async def send(message):
        sent.append(message)

    scope = {
        "type": "http", "asgi": {"version": "3.0"}, "http_version": "1.1",
        "method": method, "scheme": "http", "path": path, "raw_path": path.encode(),
        "query_string": b"", "root_path": "",
        "headers": [(b"host", b"127.0.0.1:47239")] + headers,
        "client": ("127.0.0.1", 50000), "server": ("127.0.0.1", 47239),
    }
    asyncio.run(app(scope, receive, send))
    start = next(m for m in sent if m["type"] == "http.response.start")
    body = b"".join(m.get("body", b"") for m in sent if m["type"] == "http.response.body")
    return start["status"], json.loads(body or b"{}"), calls["receive"]


def test_unauthenticated_chunked_post_is_rejected_before_reading_the_body(token_file, real_auth):
    chunk = b"x" * (1024 * 1024)
    status, body, receives = _run_asgi(
        "POST", "/analyze", [(b"content-type", b"application/json"),
                             (b"transfer-encoding", b"chunked")], [chunk] * 64)
    assert status == 401
    assert receives == 0          # not one body byte was buffered


def test_wrong_token_is_rejected_before_reading_the_body(token_file, real_auth):
    status, _, receives = _run_asgi(
        "POST", "/analyze", [(b"authorization", b"Bearer nope")], [b"{}"])
    assert status == 401 and receives == 0


def test_authenticated_chunked_body_over_the_limit_is_413_without_buffering_it_all():
    chunk = b" " * (1024 * 1024)
    n_chunks = _MAX_REQUEST_BODY_BYTES // len(chunk) + 32
    status, body, receives = _run_asgi(
        "POST", "/analyze", [(b"content-type", b"application/json")], [chunk] * n_chunks)
    assert status == 413
    assert body["detail"] == "Request body too large."
    # Stopped as soon as the running total crossed the limit.
    assert receives <= _MAX_REQUEST_BODY_BYTES // len(chunk) + 1


def test_declared_oversized_content_length_is_413():
    status, _, receives = _run_asgi(
        "POST", "/analyze", [(b"content-length", str(_MAX_REQUEST_BODY_BYTES + 1).encode())], [])
    assert status == 413 and receives == 0


def test_valid_token_passes_the_gate(token_file, real_auth):
    resp = client.get("/diagnostics/queues",
                      headers={"Authorization": "Bearer s3cret-token-value"})
    assert resp.status_code == 200


# --------------------------------------------------------------- AUDIT-076
def test_non_ascii_token_is_a_401_not_a_500(token_file):
    with pytest.raises(HTTPException) as exc:
        auth.check_bearer_token("t\u00f6ken-\u00e9")
    assert exc.value.status_code == 401


def test_non_ascii_bearer_header_is_401_over_http(token_file, real_auth):
    status, _, _ = _run_asgi("GET", "/diagnostics/queues",
                             [(b"authorization", "Bearer t\u00f6ken".encode("latin-1"))], [])
    assert status == 401


# --------------------------------------------------------------- AUDIT-073
@pytest.mark.parametrize("path", ["/docs", "/redoc", "/openapi.json"])
def test_api_docs_are_disabled_by_default(path):
    assert client.get(path).status_code == 404


def test_health_route_manifest_still_works_without_openapi_route():
    body = client.get("/health").json()
    assert "POST /analyze" in body["routes"]


def test_untrusted_host_header_is_rejected():
    resp = client.get("/health", headers={"host": "evil.example:47239"})
    assert resp.status_code == 400


def test_loopback_host_header_is_accepted():
    assert client.get("/health", headers={"host": "127.0.0.1:47239"}).status_code == 200


# --------------------------------------------------------------- AUDIT-023
_REPORT = {
    "session_name": "S", "file_name": "f.wav", "computed_at": "2026-10-15T00:00:00Z",
    "lufs_integrated": -14.0, "lra": 6.0, "true_peak_dbtp": -1.0, "crest_factor_db": 10.0,
    "dynamic_range_db": 8.0, "stereo_width": 0.5, "recommendations": [],
}


@pytest.mark.parametrize("endpoint,name", [
    ("/report", "evil.pth"), ("/report", "x.txt"), ("/report/pdf", "x.html"),
])
def test_report_writers_allowlist_their_extension(tmp_output_root, endpoint, name):
    out = tmp_output_root / name
    resp = client.post(endpoint, json={**_REPORT, "output_path": str(out)})
    assert resp.status_code == 422
    assert not out.exists()


def test_storyboard_allowlists_html(tmp_output_root):
    out = tmp_output_root / "x.pth"
    resp = client.post("/report/storyboard", json={"session_id": "s", "output_path": str(out)})
    assert resp.status_code == 422 and not out.exists()


def test_output_path_inside_config_dir_is_forbidden(tmp_path, monkeypatch):
    from khensu_services.common import config, output_paths
    cfg = tmp_path / "cfg"
    cfg.mkdir()
    monkeypatch.setattr(config, "config_dir", lambda: cfg)
    with pytest.raises(HTTPException) as exc:
        output_paths.validate_output_path(str(cfg / "settings.html"), str(cfg / "song.wav"))
    assert exc.value.status_code == 403
    assert cfg not in output_paths.allowed_output_roots()


@pytest.mark.parametrize("name", ["x.pth", "x.PTH", "x.bat", "x.pth.", "run.lnk"])
def test_executable_output_extensions_are_forbidden(tmp_path, name):
    from khensu_services.common.output_paths import validate_output_path
    with pytest.raises(HTTPException) as exc:
        validate_output_path(str(tmp_path / name), str(tmp_path / "song.wav"))
    assert exc.value.status_code == 403


def test_report_text_is_stripped_of_control_characters(tmp_output_root):
    out = tmp_output_root / "r.html"
    resp = client.post("/report", json={**_REPORT, "session_name": "A\nimport os\r\x00B",
                                        "output_path": str(out)})
    assert resp.status_code == 200
    html = out.read_text(encoding="utf-8")
    assert "A import os  B" in html
    assert "\x00" not in html


def test_strip_control_chars_recurses():
    from khensu_services.report.generator import strip_control_chars
    assert strip_control_chars({"a": ["x\ny", 3]}) == {"a": ["x y", 3]}


# --------------------------------------------------------------- AUDIT-063
@pytest.mark.parametrize("path,expected", [
    (r"\\evil\share\x.wav", True), ("//evil/share/x.wav", True),
    (r"\\?\UNC\evil\share\x.wav", True), (r"\\?\C:\x.wav", True), (r"\\.\pipe\x", True),
    ("smb://evil/x.wav", True), ("http://evil/x.wav", True), ("file://evil/x.wav", True),
    (r"C:\music\x.wav", False), ("/home/u/x.wav", False), ("rel/x.wav", False),
    (r"Z:\nas\x.wav", False), ("", False), (None, False),
])
def test_is_network_path(path, expected):
    from khensu_services.common.local_paths import is_network_path
    assert is_network_path(path) is expected


def test_unc_input_path_is_403_before_any_filesystem_access(monkeypatch):
    import os
    seen = []
    real_isfile = os.path.isfile
    monkeypatch.setattr(os.path, "isfile", lambda p: seen.append(p) or real_isfile(p))
    unc = r"\\evil-host\share\song.wav"
    resp = client.post("/analyze", json={"file_path": unc, "mode": "quick"})
    assert resp.status_code == 403
    assert unc not in seen


def test_unc_nested_and_query_paths_are_403():
    unc = "//evil-host/share/ir.wav"
    resp = client.post("/copilot/apply_chain", json={
        "file_path": "C:/nope.wav", "output_path": "C:/out.wav",
        "effects": [{"effect": "ConvolutionReverb",
                     "parameters": {"impulse_response_path": unc}}]})
    assert resp.status_code == 403


def test_require_file_rejects_unc():
    from khensu_services.common.errors import require_file
    with pytest.raises(HTTPException) as exc:
        require_file(r"\\evil\share\x.wav")
    assert exc.value.status_code == 403


def test_unc_output_path_is_403(tmp_path):
    from khensu_services.common.output_paths import validate_output_path
    with pytest.raises(HTTPException) as exc:
        validate_output_path(r"\\evil\share\out.wav", str(tmp_path / "s.wav"))
    assert exc.value.status_code == 403


def test_impulse_response_loader_rejects_unc_and_non_audio(tmp_path):
    from khensu_services.copilot.dsp import load_impulse_response
    with pytest.raises(ValueError, match="local file"):
        load_impulse_response(r"\\evil\share\ir.wav", 44100)
    bogus = tmp_path / "ir.exe"
    bogus.write_bytes(b"MZ")
    with pytest.raises(ValueError, match="audio file"):
        load_impulse_response(str(bogus), 44100)


def test_llm_suggested_network_paths_are_dropped():
    from khensu_services.common.local_paths import drop_network_path_params
    spec = {"effect": "ConvolutionReverb",
            "parameters": {"impulse_response_path": r"\\evil\s\ir.wav", "mix": 0.3}}
    assert drop_network_path_params(spec)["parameters"] == {"mix": 0.3}


# --------------------------------------------------------------- AUDIT-075
def test_unprobeable_large_file_is_rejected_by_size(tmp_path, monkeypatch):
    from khensu_services.common import audio_limits
    blob = tmp_path / "song.m4a"
    blob.write_bytes(b"\x00" * 4096)          # no header any prober understands
    monkeypatch.setattr(audio_limits, "_MAX_DECODED_AUDIO_BYTES", 4096)
    with pytest.raises(audio_limits.AudioInputLimitError, match="unreadable"):
        audio_limits.validate_audio_decode_budget(str(blob))
    # ...and the request-level guard no longer skips it.
    with pytest.raises(audio_limits.AudioInputLimitError):
        audio_limits.check_audio_inputs([str(blob)])


def test_unprobeable_small_file_is_a_probe_error(tmp_path):
    from khensu_services.common import audio_limits
    blob = tmp_path / "x.m4a"
    blob.write_bytes(b"junk")
    with pytest.raises(audio_limits.AudioInputProbeError):
        audio_limits.validate_audio_decode_budget(str(blob))
    # The guard surfaces it too (-> 422) instead of leaving the endpoint to
    # fail the decode as a 500 (bad-input sweep 2026-10-16).
    with pytest.raises(audio_limits.AudioInputProbeError):
        audio_limits.check_audio_inputs([str(blob)])


def test_mutagen_header_is_used_when_libsndfile_cannot_probe(tmp_path, monkeypatch):
    from khensu_services.common import audio_limits

    class _Info:
        length = 7200.0
        sample_rate = 44100
        channels = 2

    class _Meta:
        info = _Info()

    import mutagen
    blob = tmp_path / "long.m4a"
    blob.write_bytes(b"junk")
    monkeypatch.setattr(mutagen, "File", lambda p: _Meta())
    with pytest.raises(audio_limits.AudioInputLimitError, match="duration"):
        audio_limits.validate_audio_decode_budget(str(blob))


# --------------------------------------------------------------- AUDIT-057
def _seed_legacy(path, rows):
    c = sqlite3.connect(str(path))
    c.execute("CREATE TABLE recommendation_feedback (id TEXT PRIMARY KEY, "
              "recommendation_id TEXT NOT NULL, acted_on INTEGER, feedback_note TEXT, "
              "created_at TEXT, updated_at TEXT)")
    c.executemany("INSERT INTO recommendation_feedback VALUES (?,?,?,?,?,?)", rows)
    c.commit()
    c.close()


def test_failed_legacy_import_keeps_the_store_working_and_retries(monkeypatch, tmp_path):
    monkeypatch.delenv("KHENSU_FEEDBACK_DB", raising=False)
    monkeypatch.setattr(auth, "get_token_path", lambda: tmp_path / "token.txt")
    legacy = tmp_path / "service_feedback.db"
    legacy.write_bytes(b"this is not a sqlite database" * 100)

    import khensu_services.feedback.db as fdb
    old = fdb._conn
    fdb._conn = None
    try:
        out = fdb.upsert_feedback("r1", True, "works")       # import fails -> no 500
        assert out["recommendation_id"] == "r1"
        conn = fdb._conn
        assert conn is not None and fdb._import_pending
        assert fdb._get_conn() is conn                       # same handle, no leak

        legacy.unlink()
        _seed_legacy(legacy, [("i1", "rLegacy", 1, "old", "t0", "t0")])
        fdb._next_import_at = 0.0                            # retry window elapsed
        assert fdb._get_conn() is conn
        assert not fdb._import_pending
        ids = {r[0] for r in conn.execute("SELECT recommendation_id FROM recommendation_feedback")}
        assert ids == {"r1", "rLegacy"}
    finally:
        if fdb._conn is not None:
            fdb._conn.close()
        fdb._conn = old
        fdb._import_pending = False


# --------------------------------------------------------------- AUDIT-059
def test_import_carries_uncheckpointed_wal_rows_and_sidecars(tmp_path):
    import shutil

    from khensu_services.common import db_unify
    live = tmp_path / "live.db"
    c = sqlite3.connect(str(live))
    c.execute("PRAGMA journal_mode=WAL")
    c.execute("PRAGMA wal_autocheckpoint=0")
    c.execute("CREATE TABLE recommendation_feedback (id TEXT PRIMARY KEY, "
              "recommendation_id TEXT NOT NULL, acted_on INTEGER, feedback_note TEXT, "
              "created_at TEXT, updated_at TEXT)")
    c.commit()
    c.execute("INSERT INTO recommendation_feedback VALUES ('i1','rWal',1,'in-wal','t0','t0')")
    c.commit()
    # Snapshot main + WAL while the writer is still open: the row lives only
    # in the -wal sidecar, exactly what an un-checkpointed legacy file is.
    legacy = tmp_path / "service_feedback.db"
    shutil.copy2(live, legacy)
    shutil.copy2(str(live) + "-wal", str(legacy) + "-wal")
    c.close()

    dest = sqlite3.connect(str(tmp_path / "khensu.db"))
    dest.execute("CREATE TABLE recommendation_feedback (id TEXT PRIMARY KEY, "
                 "recommendation_id TEXT NOT NULL UNIQUE, acted_on INTEGER, feedback_note TEXT, "
                 "created_at TEXT, updated_at TEXT)")
    dest.commit()
    assert db_unify.import_once(dest, "fb", str(legacy),
                                lambda conn: conn.execute(
                                    "INSERT INTO recommendation_feedback "
                                    "SELECT * FROM src.recommendation_feedback")) == "imported"
    assert dest.execute("SELECT feedback_note FROM recommendation_feedback").fetchone()[0] == "in-wal"
    assert (tmp_path / "service_feedback.db.bak-wal").exists()     # sidecar backed up
    assert not (tmp_path / "service_feedback.db-wal").exists()     # nothing left behind
    migrated = sqlite3.connect(str(tmp_path / "service_feedback.db.migrated"))
    try:                                                           # retired copy is complete
        assert migrated.execute("SELECT COUNT(*) FROM recommendation_feedback").fetchone()[0] == 1
    finally:
        migrated.close()
    dest.close()


def test_backup_failure_aborts_the_import(tmp_path, monkeypatch):
    from khensu_services.common import db_unify
    legacy = tmp_path / "service_feedback.db"
    _seed_legacy(legacy, [("i1", "r1", 1, "n", "t0", "t0")])
    dest = sqlite3.connect(str(tmp_path / "khensu.db"))

    def failing_copy(src, dst):
        open(dst, "wb").close()              # truncated backup

    monkeypatch.setattr(db_unify.shutil, "copy2", failing_copy)
    with pytest.raises(OSError, match="incomplete"):
        db_unify.import_once(dest, "fb", str(legacy), lambda conn: None)
    assert legacy.exists()                   # original untouched, marker unset
    assert not dest.execute("SELECT 1 FROM data_migrations WHERE name='fb'").fetchone()
    dest.close()


# --------------------------------------------------------------- AUDIT-060
def test_upsert_is_atomic_against_a_concurrent_writer(monkeypatch, tmp_path):
    db = tmp_path / "u.db"
    monkeypatch.setenv("KHENSU_FEEDBACK_DB", str(db))
    import khensu_services.feedback.db as fdb
    old = fdb._conn
    fdb._conn = None
    try:
        fdb._get_conn()
        # The desktop writer inserts the same recommendation_id first.
        other = sqlite3.connect(str(db))
        other.execute("INSERT INTO recommendation_feedback VALUES "
                      "('cpp-id','rX',0,'from-cpp','t0','t0')")
        other.commit()
        other.close()
        out = fdb.upsert_feedback("rX", True, "from-py")   # no IntegrityError
        assert out["id"] == "cpp-id" and out["feedback_note"] == "from-py"
        assert fdb._get_conn().execute(
            "SELECT COUNT(*) FROM recommendation_feedback").fetchone()[0] == 1
    finally:
        if fdb._conn is not None:
            fdb._conn.close()
        fdb._conn = old


# --------------------------------------------------------------- AUDIT-074
def test_distinct_non_latin_names_get_distinct_stem_folders():
    from khensu_services.util.naming import amas_folder_name
    d = datetime.date(2026, 7, 10)
    a = amas_folder_name(r"C:\m\Песня.wav", d)
    b = amas_folder_name(r"C:\m\Трек.wav", d)
    assert a != b
    assert a.startswith("AMAS_7_10_2026_Untitled_") and len(a.rsplit("_", 1)[1]) == 8


def test_ascii_names_keep_the_spec_format():
    from khensu_services.util.naming import amas_folder_name
    d = datetime.date(2026, 7, 10)
    assert amas_folder_name(r"C:\m\Shadow in my room.wav", d) == "AMAS_7_10_2026_Shadowinmyroom"


def test_lossy_name_hash_is_fnv1a32_of_the_utf8_stem():
    # Pinned so the C++ mirror (StemService::getStemsDirectory) can't drift.
    from khensu_services.util.naming import _fnv1a32_hex, amas_folder_name
    assert _fnv1a32_hex("a") == "e40c292c"
    d = datetime.date(2026, 7, 10)
    assert amas_folder_name("Tr\u00e4ck.wav", d) == "AMAS_7_10_2026_Trck_" + _fnv1a32_hex("Tr\u00e4ck")


# --------------------------------------------------------------- AUDIT-077
def test_bootstrap_does_not_download_when_master_online_is_off(tmp_path, monkeypatch, capsys):
    import launch_service
    from khensu_services.common import online
    settings = tmp_path / "settings.json"
    settings.write_text(json.dumps({"online": {"enabled": False}}), encoding="utf-8")
    monkeypatch.setattr(online, "settings_path", lambda: settings)
    online._invalidate_cache()
    calls = []
    try:
        assert launch_service.bootstrap_venv(run=lambda *a, **k: calls.append(a)) is False
    finally:
        online._invalidate_cache()
    assert calls == []
    assert "online features are switched off" in capsys.readouterr().err


def test_bootstrap_fails_closed_on_corrupt_settings(tmp_path, monkeypatch):
    import launch_service
    from khensu_services.common import online
    settings = tmp_path / "settings.json"
    settings.write_text("{not json", encoding="utf-8")
    monkeypatch.setattr(online, "settings_path", lambda: settings)
    online._invalidate_cache()
    try:
        assert launch_service.bootstrap_online_allowed() is False
    finally:
        online._invalidate_cache()


def test_bootstrap_downloads_when_online(tmp_path, monkeypatch):
    import launch_service
    from khensu_services.common import online
    monkeypatch.setattr(online, "settings_path", lambda: tmp_path / "missing.json")
    online._invalidate_cache()
    calls = []
    try:
        assert launch_service.bootstrap_venv(run=lambda *a, **k: calls.append(a[0])) is True
    finally:
        online._invalidate_cache()
    assert any("--require-hashes" in c for c in calls)


def test_bootstrap_midi_extra_is_best_effort(tmp_path, monkeypatch, capsys):
    """The [midi] extra (python-rtmidi, MMC Locate) installs after the base
    runtime; its failure leaves MMC off and never fails the setup."""
    import types
    import launch_service
    from khensu_services.common import online
    monkeypatch.setattr(online, "settings_path", lambda: tmp_path / "missing.json")
    online._invalidate_cache()
    calls = []

    def run(cmd, **kw):
        calls.append(cmd)
        failed = any("requirements-midi-hashed.txt" in str(c) for c in cmd)
        if failed and kw.get("check"):
            raise AssertionError("the midi install must not be check=True")
        return types.SimpleNamespace(returncode=1 if failed else 0)
    try:
        assert launch_service.bootstrap_venv(run=run) is True
    finally:
        online._invalidate_cache()
    assert any(any("requirements-midi-hashed.txt" in str(c) for c in cmd) for cmd in calls)
    assert "continuing without it" in capsys.readouterr().out


# --------------------------------------------------------------- AUDIT-087
def test_matcher_waits_out_a_brief_writer_lock(tmp_path, monkeypatch):
    from khensu_services.copilot import matcher
    db = tmp_path / "khensu.db"
    setup = sqlite3.connect(str(db))
    setup.execute("CREATE TABLE scanned_plugins (id TEXT, name TEXT, manufacturer TEXT, "
                  "category TEXT, format TEXT, file_path TEXT)")
    setup.execute("INSERT INTO scanned_plugins VALUES ('1','Pro-Q','FabFilter','EQ','VST3','x')")
    setup.commit()
    setup.close()
    monkeypatch.setattr(matcher, "get_token_path", lambda: tmp_path / "token.txt")

    locker = sqlite3.connect(str(db), check_same_thread=False)
    locker.execute("BEGIN EXCLUSIVE")
    timer = threading.Timer(0.5, locker.commit)
    timer.start()
    try:
        plugins = matcher.load_scanned_plugins()
    finally:
        timer.join()
        locker.close()
    assert [p["name"] for p in plugins] == ["Pro-Q"]


def test_wav_fixture_helper_sanity(tmp_path):
    # Guard: a plain local WAV still passes every new path check end to end.
    path = tmp_path / "ok.wav"
    sf.write(str(path), np.zeros(800, dtype=np.float32), 8000)
    from khensu_services.common.errors import check_decode_budget
    check_decode_budget(str(path))
