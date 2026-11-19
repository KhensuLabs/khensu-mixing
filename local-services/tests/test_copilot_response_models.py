"""Typed /copilot response models (the generated-C++-client contract).

The job queues' completion paths are exercised end to end in
test_copilot_job_queues / test_platform_render / test_apply_chain_batch;
these cover the routes nothing else hit under strict response models, plus
the stored-payload shapes (interrupted, guardrail-rejected) a poll can return.
"""
from __future__ import annotations

import sqlite3

import pytest

from khensu_services.common.job_status import JobStatus


def test_model_status_matches_model(client):
    r = client.get("/copilot/model_status")
    assert r.status_code == 200
    body = r.json()
    for key in ("status", "installed", "downloaded_bytes", "total_bytes", "progress",
                "speed_kbps", "demucs_package_available"):
        assert key in body


def test_model_status_keeps_integers_integers(client, monkeypatch):
    import khensu_services.copilot.downloader as dl
    monkeypatch.setattr(dl.ModelDownloader, "get_progress", lambda self: {
        "status": "downloading", "installed": False, "downloaded_bytes": 1024,
        "total_bytes": 4096, "progress": 0.25, "speed_kbps": 0.0, "eta_seconds": 0,
        "error_message": "", "models_dir": "C:/m", "demucs_package_available": False})
    body = client.get("/copilot/model_status").json()
    # int | float, not float: the wire must not turn 1024 into 1024.0.
    assert isinstance(body["downloaded_bytes"], int) and body["downloaded_bytes"] == 1024
    assert isinstance(body["eta_seconds"], int)


@pytest.mark.parametrize("path, method", [("/copilot/pause_download", "pause"),
                                           ("/copilot/stop_download", "stop")])
def test_pause_and_stop_download(client, monkeypatch, path, method):
    import khensu_services.copilot.downloader as dl
    calls = []
    monkeypatch.setattr(dl.ModelDownloader, method, lambda self: calls.append(method))
    r = client.post(path)
    assert r.status_code == 200
    assert r.json() == {"status": "success"}
    assert calls == [method]


def test_scanned_plugins_rows(client, monkeypatch, tmp_path):
    import khensu_services.copilot.matcher as matcher
    db = tmp_path / "khensu.db"
    with sqlite3.connect(db) as conn:
        conn.execute("CREATE TABLE scanned_plugins (id INTEGER PRIMARY KEY, name TEXT, "
                     "manufacturer TEXT, category TEXT, format TEXT, file_path TEXT)")
        conn.execute("INSERT INTO scanned_plugins VALUES (7, 'Pro-Q 3', 'FabFilter', 'EQ', "
                     "'VST3', 'C:/p.vst3')")
        conn.execute("INSERT INTO scanned_plugins VALUES (8, 'Odd', NULL, NULL, 'VST3', NULL)")
    monkeypatch.setattr(matcher, "get_khensu_db_path", lambda: db)
    rows = matcher.load_scanned_plugins()
    assert rows[0] == {"id": 7, "name": "Pro-Q 3", "manufacturer": "FabFilter",
                       "category": "EQ", "format": "VST3", "file_path": "C:/p.vst3"}
    # A NULL column stays null rather than failing the response.
    assert rows[1]["manufacturer"] is None


def test_interrupted_render_job_status(client):
    from khensu_services.copilot import router_render
    router_render._apply_jobs["job-int"] = {
        "status": JobStatus.INTERRUPTED.value, "result": None,
        "error": "Service shutdown interrupted this job"}
    try:
        body = client.get("/copilot/apply_chain/status/job-int").json()
        assert body == {"status": "interrupted", "result": None,
                        "error": "Service shutdown interrupted this job"}
    finally:
        router_render._apply_jobs.pop("job-int", None)


def test_guardrail_rejected_render_result_keeps_override_inputs(client):
    from khensu_services.copilot import router_render_sections
    result = {"status": "rejected", "guardrail_status": "true_peak", "render_state": "rejected",
              "quarantine_path": "C:/q/.quarantine_out.wav", "measurements": {"ok": False},
              "source_path": "C:/in.wav", "output_path": "C:/out.wav"}
    router_render_sections._section_chain_jobs["job-rej"] = {
        "status": "failed", "error": "rejected", "result": result}
    try:
        body = client.get("/copilot/apply_section_chain/status/job-rej").json()
        assert body["result"] == result
    finally:
        router_render_sections._section_chain_jobs.pop("job-rej", None)


def test_cancel_responses_are_typed(client):
    for path in ("/copilot/apply_chain/x/cancel", "/copilot/apply_chain/batch/x/cancel",
                 "/copilot/apply_section_chain/x/cancel", "/copilot/generate_mixer_chain/x/cancel",
                 "/copilot/mastering_agent/x/cancel", "/copilot/platform_masters/x/cancel",
                 "/copilot/chat/x/cancel"):
        r = client.post(path)
        assert r.status_code == 200, path
        assert r.json() == {"ok": False, "status": "unknown"}, path   # SVC-14


def test_every_copilot_json_route_declares_a_model():
    from khensu_services.server import app
    untyped = []
    for path, methods in app.openapi()["paths"].items():
        if not path.startswith("/copilot"):
            continue
        for method, op in methods.items():
            content = op.get("responses", {}).get("200", {}).get("content", {})
            if "application/json" not in content:
                continue   # streams declare their own media type
            schema = content["application/json"].get("schema", {})
            if "$ref" not in schema and "$ref" not in schema.get("items", {}):
                untyped.append(f"{method.upper()} {path}")
    assert untyped == []


def test_chat_stream_declares_event_stream():
    from khensu_services.server import app
    content = app.openapi()["paths"]["/copilot/chat/stream"]["post"]["responses"]["200"]["content"]
    assert "text/event-stream" in content
