"""Regression tests for the 2026-11-06 audit, service-infrastructure slice
(SVC-02/04/07/08/09/11/12/14/15/16/17/19, WIRE-11, S11)."""
from __future__ import annotations

import asyncio
import json
import os
import threading
import time
from datetime import datetime, timedelta, timezone

import pytest

from khensu_services.analysis import router_transport as rt


@pytest.fixture
def fresh_transport(client):
    client.app.state.transport = rt.TransportState()
    yield client
    client.app.state.transport = rt.TransportState()


# ------------------------------------------------------------ SVC-16 / PLG-04
def test_playhead_is_kept_per_instance(fresh_transport):
    c = fresh_transport
    assert c.post("/transport/playhead", json={"instance_id": "vocal", "position_sec": 1.0,
                                               "correlation": 0.25}).json() == \
        {"status": "ok", "instance_id": "vocal"}
    time.sleep(0.01)
    c.post("/transport/playhead", json={"instance_id": "master", "position_sec": 9.0,
                                        "correlation": -0.5})

    vocal = c.get("/transport/playhead", params={"instance_id": "vocal"}).json()
    assert vocal["instance_id"] == "vocal" and vocal["correlation"] == 0.25
    assert vocal["position_sec"] == 1.0 and vocal["connected"] is True

    # Default: the most recently updated instance.
    latest = c.get("/transport/playhead").json()
    assert latest["instance_id"] == "master" and latest["correlation"] == -0.5
    assert latest["instances"] == ["master", "vocal"]


def test_playhead_unknown_instance_is_disconnected_defaults(fresh_transport):
    body = fresh_transport.get("/transport/playhead", params={"instance_id": "nope"}).json()
    assert body["connected"] is False and body["age_ms"] is None
    assert body["instance_id"] == "nope" and body["instances"] == []


def test_legacy_playhead_without_instance_id_still_works(fresh_transport):
    c = fresh_transport
    c.post("/transport/playhead", json={"position_sec": 3.0})
    body = c.get("/transport/playhead").json()
    assert body["position_sec"] == 3.0 and body["connected"] is True
    assert body["instance_id"] is None
    assert body["instances"] == []          # the legacy slot has no id to list


def test_instances_list_drops_stale_ids(fresh_transport, monkeypatch):
    c = fresh_transport
    c.post("/transport/playhead", json={"instance_id": "old"})
    real = rt.time.monotonic
    monkeypatch.setattr(rt.time, "monotonic", lambda: real() + rt.INSTANCE_LIVE_SEC + 1.0)
    c.post("/transport/playhead", json={"instance_id": "new"})
    assert c.get("/transport/playhead").json()["instances"] == ["new"]


@pytest.mark.parametrize("bad", ["", "a b", "x" * 129, 5, "../etc"])
def test_playhead_rejects_bad_instance_id(fresh_transport, bad):
    r = fresh_transport.post("/transport/playhead", json={"instance_id": bad})
    assert r.status_code == 422


def test_instance_count_is_bounded(fresh_transport):
    c = fresh_transport
    for i in range(rt._MAX_INSTANCES + 10):
        c.post("/transport/playhead", json={"instance_id": f"i{i}"})
    assert len(c.app.state.transport.playheads) == rt._MAX_INSTANCES


def test_seek_generation_is_seen_by_every_instance(fresh_transport, monkeypatch):
    monkeypatch.setattr(rt, "_dispatch_mmc", lambda pos: None)
    c = fresh_transport
    r = c.post("/transport/seek", params={"pos": 12.5}).json()
    assert r["generation"] == 1 and r["position_sec"] == 12.5
    # Two instances both see it: the since= read does not consume.
    for _ in range(2):
        body = c.get("/transport/seek", params={"since": 0}).json()
        assert body == {"position_sec": 12.5, "pending": True, "generation": 1}
    assert c.get("/transport/seek", params={"since": 1}).json()["pending"] is False
    c.post("/transport/seek", params={"pos": 20.0})
    assert c.get("/transport/seek", params={"since": 1}).json() == \
        {"position_sec": 20.0, "pending": True, "generation": 2}


def test_seek_without_since_keeps_legacy_consume_semantics(fresh_transport, monkeypatch):
    monkeypatch.setattr(rt, "_dispatch_mmc", lambda pos: None)
    c = fresh_transport
    c.post("/transport/seek", params={"pos": 4.0})
    assert c.get("/transport/seek").json()["pending"] is True
    assert c.get("/transport/seek").json()["pending"] is False
    assert c.get("/transport/seek", params={"since": -1}).status_code == 422


def test_params_targeting(fresh_transport):
    c = fresh_transport
    g1 = c.post("/transport/params", json={"params": {"a": 1.0}}).json()
    assert g1 == {"status": "ok", "generation": 1, "target_instance_id": None}
    g2 = c.post("/transport/params", json={"params": {"b": 2.0},
                                           "target_instance_id": "vocal"}).json()
    assert g2["generation"] == 2 and g2["target_instance_id"] == "vocal"

    vocal = c.get("/transport/params", params={"instance_id": "vocal"}).json()
    assert vocal == {"params": {"b": 2.0}, "generation": 2, "target_instance_id": "vocal"}
    # Another instance only sees the untargeted push.
    master = c.get("/transport/params", params={"instance_id": "master"}).json()
    assert master == {"params": {"a": 1.0}, "generation": 1, "target_instance_id": None}
    # A legacy poller (no id) never receives a targeted push.
    assert c.get("/transport/params").json()["params"] == {"a": 1.0}
    # A newer untargeted push supersedes the targeted one for "vocal".
    c.post("/transport/params", json={"params": {"c": 3.0}})
    assert c.get("/transport/params", params={"instance_id": "vocal"}).json()["generation"] == 3


def test_params_empty_state(fresh_transport):
    assert fresh_transport.get("/transport/params").json() == \
        {"params": {}, "generation": 0, "target_instance_id": None}
    assert fresh_transport.post("/transport/params",
                                json={"params": {}, "target_instance_id": 7}).status_code == 422


# ------------------------------------------------------------ WIRE-11 / SVC-04
def test_seek_does_not_wait_on_mmc_or_use_the_dsp_pool(fresh_transport, monkeypatch):
    from khensu_services.common import executors

    started = threading.Event()
    release = threading.Event()
    seen_threads: list[str] = []

    def slow_locate(pos):
        seen_threads.append(threading.current_thread().name)
        started.set()
        release.wait(5)
        return True

    monkeypatch.setattr(rt, "send_mmc_locate", slow_locate)
    blockers = [executors.dsp_executor.submit(release.wait, 5)
                for _ in range(executors.dsp_executor._max_workers)]
    try:
        t0 = time.monotonic()
        r = fresh_transport.post("/transport/seek", params={"pos": 1.0})
        assert r.status_code == 200
        assert time.monotonic() - t0 < 1.0     # never waited on MMC or the full DSP pool
        assert started.wait(2)
        assert seen_threads[0].startswith("khensu-io")
    finally:
        release.set()
        for b in blockers:
            b.result(5)


def test_decode_budget_probe_runs_on_the_io_pool(tmp_path, monkeypatch):
    from khensu_services.common import errors

    seen: list[str] = []
    monkeypatch.setattr(errors, "check_decode_budget",
                        lambda p: seen.append(threading.current_thread().name))
    f = tmp_path / "x.wav"
    f.write_bytes(b"0")
    asyncio.run(errors.require_decode_budget(str(f)))
    assert seen and seen[0].startswith("khensu-io")


def test_guard_skips_path_free_bodies():
    from khensu_services.common.audio_limits import mentions_audio_inputs
    assert not mentions_audio_inputs({"position_sec": 1.0, "is_playing": True})
    assert not mentions_audio_inputs({"file_path": ""})
    assert mentions_audio_inputs({"file_path": "a.wav"})
    assert mentions_audio_inputs({"x": [{"stems": {"v": "a.wav"}}]})


def test_long_job_pool_is_separate_and_bounded():
    from khensu_services.common import executors
    assert executors.long_job_executor is not executors.dsp_executor
    assert executors.long_job_executor._max_workers == 2
    assert executors.io_executor not in (executors.dsp_executor, executors.long_job_executor)
    assert all(ex in executors.ALL_EXECUTORS for ex in (
        executors.dsp_executor, executors.long_job_executor, executors.io_executor,
        executors.separation_executor, executors.llm_executor))


def test_album_master_runs_on_the_long_job_pool():
    from khensu_services.analysis import router_album
    from khensu_services.common.executors import long_job_executor
    assert router_album.long_job_executor is long_job_executor


# ------------------------------------------------------------ SVC-02
def test_shutdown_executors_cancels_queued_work():
    from concurrent.futures import ThreadPoolExecutor
    from khensu_services.common import executors

    pool = ThreadPoolExecutor(max_workers=1)
    gate = threading.Event()
    running = pool.submit(gate.wait, 5)
    queued = pool.submit(lambda: "ran")
    original = executors.ALL_EXECUTORS
    executors.ALL_EXECUTORS = (pool,)
    try:
        executors.shutdown_executors()
    finally:
        executors.ALL_EXECUTORS = original
        gate.set()
    assert queued.cancelled()
    assert running.result(5) is True


def test_request_exit_prefers_the_server_handle(monkeypatch):
    from khensu_services.common import lifecycle

    class _Server:
        should_exit = False

    server = _Server()
    monkeypatch.setattr(lifecycle, "_server", server)
    assert lifecycle.request_exit() == "server"
    assert server.should_exit is True


def test_request_exit_falls_back_to_sigint_not_kill(monkeypatch):
    import signal
    from khensu_services.common import lifecycle

    raised: list[int] = []
    monkeypatch.setattr(lifecycle, "_server", None)
    monkeypatch.setattr(lifecycle.signal, "raise_signal", lambda s: raised.append(s))
    assert lifecycle.request_exit() == "signal"
    assert raised == [signal.SIGINT]


def test_shutdown_route_uses_graceful_exit(client, monkeypatch):
    from khensu_services.common import jobs, lifecycle

    calls: list[str] = []
    monkeypatch.setattr(lifecycle, "request_exit", lambda: calls.append("x") or "server")
    monkeypatch.setattr(jobs, "interrupt_all_jobs", lambda: 0)
    monkeypatch.setattr(client.app.state, "shutting_down", False, raising=False)
    assert client.post("/shutdown").status_code == 200
    assert calls == ["x"]


def test_launcher_builds_a_registered_server(monkeypatch):
    import launch_service
    import uvicorn
    from khensu_services.common import lifecycle

    captured = {}

    class _FakeServer:
        started = True

        def __init__(self, config):
            captured["config"] = config

        def run(self):
            captured["registered"] = lifecycle.get_server() is self

    monkeypatch.setattr(uvicorn, "Server", _FakeServer)
    exits: list = []
    monkeypatch.setattr(launch_service, "_exit_after_serve", lambda server: exits.append(server))
    try:
        launch_service.run_service()
    finally:
        lifecycle.set_server(None)
    assert captured["registered"] is True
    assert captured["config"].timeout_graceful_shutdown == lifecycle.GRACEFUL_REQUEST_TIMEOUT_SEC
    assert len(exits) == 1


# ------------------------------------------------------------ SVC-15
def test_corrupt_settings_force_hf_offline():
    from launch_service import apply_offline_env
    env: dict[str, str] = {}
    apply_offline_env(env, {}, corrupt=True)
    assert env.get("HF_HUB_OFFLINE") == "1" and env.get("TRANSFORMERS_OFFLINE") == "1"
    env2: dict[str, str] = {}
    apply_offline_env(env2, {"online": "garbage"})
    assert env2.get("HF_HUB_OFFLINE") == "1"


# ------------------------------------------------------------ SVC-12
def test_health_hides_route_manifest_from_unauthenticated_callers(client, monkeypatch):
    from khensu_services import auth
    from khensu_services.auth import verify_token

    app = client.app
    override = app.dependency_overrides.pop(verify_token)
    events: list[tuple] = []
    monkeypatch.setattr(auth, "record_event", lambda *a, **k: events.append(a))
    try:
        body = client.get("/health").json()
        assert "routes" not in body and "route_count" not in body
        assert body["version"] and body["service"] == "khensu-services"
        assert events == []                 # a token-less probe is not an auth failure
        bad = client.get("/health", headers={"Authorization": "Bearer wrong"}).json()
        assert "routes" not in bad
    finally:
        app.dependency_overrides[verify_token] = override
    authed = client.get("/health").json()
    assert authed["route_count"] == len(authed["routes"]) > 0


def test_health_still_answers_nonce_proof_without_auth(client, monkeypatch):
    import hashlib
    import hmac
    from khensu_services import server
    from khensu_services.auth import verify_token

    monkeypatch.setattr(server, "read_local_token", lambda: "tok")
    override = client.app.dependency_overrides.pop(verify_token)
    try:
        nonce = "ab" * 16
        body = client.get("/health", params={"nonce": nonce}).json()
    finally:
        client.app.dependency_overrides[verify_token] = override
    assert body["proof"] == hmac.new(b"tok", nonce.encode(), hashlib.sha256).hexdigest()


# ------------------------------------------------------------ SVC-09
def test_token_read_failure_does_not_leak_os_error(monkeypatch):
    from fastapi import HTTPException
    from khensu_services import auth

    def boom():
        raise PermissionError(r"C:\Users\someone\AppData\Roaming\KhensuMixing\token.txt denied")

    monkeypatch.setattr(auth, "read_local_token", boom)
    with pytest.raises(HTTPException) as info:
        auth.check_bearer_token("x")
    assert info.value.status_code == 500
    assert "someone" not in info.value.detail and "token.txt" not in info.value.detail


# ------------------------------------------------------------ SVC-07 / SVC-08 / SVC-14
def _tone(path, sec=0.5):
    import numpy as np
    import soundfile as sf
    sr = 22050
    t = np.arange(int(sr * sec)) / sr
    sf.write(path, 0.2 * np.sin(2 * np.pi * 220 * t), sr)
    return str(path)


class _NoRun:
    def submit(self, *a, **k):
        from concurrent.futures import Future
        return Future()


def test_separate_dedup_includes_source_stat(client, tmp_path, monkeypatch):
    from khensu_services.analysis import router_separate as rs
    monkeypatch.setattr(rs, "_separation_executor", _NoRun())
    src = _tone(tmp_path / "mix.wav")
    body = {"file_path": src, "output_dir": str(tmp_path / "stems"), "method": "hpss"}
    first = client.post("/separate/start", json=body).json()["job_id"]
    assert client.post("/separate/start", json=body).json()["job_id"] == first
    # Re-rendered in place: different size/mtime -> a new job, not a join.
    _tone(tmp_path / "mix.wav", sec=0.7)
    st = os.stat(src)
    os.utime(src, ns=(st.st_atime_ns, st.st_mtime_ns + 10_000_000))
    second = client.post("/separate/start", json=body).json()["job_id"]
    assert second != first
    for jid in (first, second):
        client.post(f"/separate/{jid}/cancel")


@pytest.mark.parametrize("terminal,status", [
    ("failed", 500), ("cancelled", 409), ("interrupted", 503), ("vanished", 404)])
def test_legacy_separate_maps_every_terminal_state(client, tmp_path, monkeypatch, terminal, status):
    from khensu_services.analysis import router_separate as rs

    async def fake_start(req):  # separate_start is an io_route (async)
        job_id = f"legacy-{terminal}"
        if terminal != "vanished":
            rs._jobs[job_id] = {"status": terminal, "result": None,
                                "error": "Traceback: secret C:\\path\\detail"}
        return {"job_id": job_id}

    monkeypatch.setattr(rs, "separate_start", fake_start)
    src = _tone(tmp_path / "m.wav")
    t0 = time.monotonic()
    r = client.post("/separate", json={"file_path": src, "output_dir": str(tmp_path / "o")})
    assert r.status_code == status
    assert time.monotonic() - t0 < 5      # never the 300 s spin
    assert "secret" not in r.text


def test_album_cancel_unknown_reports_ok_false(client):
    assert client.post("/album/master/nope/cancel").json() == {"ok": False, "status": "unknown"}
    assert client.get("/album/master/status/nope").status_code == 404


def test_make_job_routes_factory():
    from concurrent.futures import Future
    from fastapi import APIRouter, FastAPI
    from fastapi.testclient import TestClient
    from khensu_services.common.job_models import JobCancelResponse, StageJobStatusResponse
    from khensu_services.common.jobs import JobStore, make_job_routes

    router = APIRouter()
    store = JobStore(name="factory_test", max_active=2)
    futures: dict[str, Future] = {}
    make_job_routes(router, store, status_path="/t/status/{job_id}", cancel_path="/t/{job_id}/cancel",
                    status_model=StageJobStatusResponse, cancel_model=JobCancelResponse, futures=futures)
    app = FastAPI()
    app.include_router(router)
    c = TestClient(app)
    store["j1"] = {"status": "pending"}
    fut: Future = Future()
    futures["j1"] = fut
    assert c.get("/t/status/j1").json() == {"status": "pending"}
    assert c.get("/t/status/zz").status_code == 404
    assert c.post("/t/j1/cancel").json() == {"ok": True, "status": "cancelled"}
    assert fut.cancelled()
    assert c.post("/t/zz/cancel").json() == {"ok": False, "status": "unknown"}
    with pytest.raises(ValueError):
        make_job_routes(router, store, status_path="/x", cancel_path="/y/{job_id}",
                        status_model=StageJobStatusResponse)


# ------------------------------------------------------------ SVC-17
def test_album_master_rejects_more_than_99_tracks_before_probing(client, monkeypatch):
    from khensu_services.analysis import router_album

    probed: list[str] = []
    monkeypatch.setattr(router_album, "_require_tracks", lambda paths: probed.extend(paths))
    tracks = [{"path": f"t{i}.wav"} for i in range(100)]
    for route in ("/album/master/start", "/album/master/plan"):
        assert client.post(route, json={"tracks": tracks}).status_code == 422
    assert probed == []


# ------------------------------------------------------------ SVC-11
def test_model_update_fetch_requires_https():
    from khensu_services.analysis import model_updates
    with pytest.raises(ValueError):
        model_updates._fetch_remote_manifest("http://example.com/m.json")


def test_model_update_fetch_uses_safe_client(monkeypatch):
    from khensu_services.analysis import model_updates
    from khensu_services.common import http_safety

    used = {}

    class _Resp:
        content = b'{"version": "2"}'

        def raise_for_status(self):
            pass

    class _Client:
        def __init__(self, timeout):
            used["timeout"] = timeout

        def __enter__(self):
            return self

        def __exit__(self, *a):
            return False

        def get(self, url):
            used["url"] = url
            return _Resp()

    monkeypatch.setattr(http_safety, "safe_httpx_client", lambda timeout=30.0: _Client(timeout))
    assert model_updates._fetch_remote_manifest("https://example.com/m.json") == {"version": "2"}
    assert used["url"] == "https://example.com/m.json"


def test_cross_host_redirect_is_refused():
    import httpx
    from khensu_services.common.http_safety import CrossHostRedirectError, _reject_cross_host_redirect
    req = httpx.Request("GET", "https://models.example.com/m.json")
    resp = httpx.Response(302, headers={"location": "https://evil.example.net/x"}, request=req)
    with pytest.raises(CrossHostRedirectError):
        _reject_cross_host_redirect(resp)


# ------------------------------------------------------------ SVC-19
def test_provenance_events_retention(tmp_path, monkeypatch):
    from khensu_services.events import db

    monkeypatch.setattr(db, "_conn", None)
    monkeypatch.setattr(db, "_import_pending", False)
    monkeypatch.setattr(db, "_appends_since_prune", 0)
    monkeypatch.setenv("KHENSU_EVENTS_DB", str(tmp_path / "events.db"))
    conn = db._get_conn()
    try:
        old = (datetime.now(timezone.utc) - timedelta(days=400)).isoformat()
        conn.execute("INSERT INTO provenance_events (id, event_type, created_at) VALUES (?,?,?)",
                     ("old", "export", old))
        conn.commit()
        for i in range(5):
            db.append_event("s", "export", summary=str(i), payload={"i": i})
        assert db.prune_events(max_age_days=365, max_rows=0) == 1
        assert db.prune_events(max_age_days=0, max_rows=3) == 2
        kept = [e["summary"] for e in db.list_events()]
        assert sorted(kept) == ["2", "3", "4"]
        # Retention also runs automatically every PRUNE_EVERY_APPENDS appends.
        monkeypatch.setattr(db, "PRUNE_EVERY_APPENDS", 2)
        monkeypatch.setattr(db, "_appends_since_prune", 0)
        monkeypatch.setenv("KHENSU_EVENTS_MAX_ROWS", "2")
        db.append_event("s", "export", summary="5")
        db.append_event("s", "export", summary="6")
        assert sorted(e["summary"] for e in db.list_events()) == ["5", "6"]
    finally:
        conn.close()
        monkeypatch.setattr(db, "_conn", None)


# ------------------------------------------------------------ S11
def test_cleaned_routers_use_require_file_404(client, tmp_path):
    missing = str(tmp_path / "nope.wav")
    for route, body in [
        ("/analyze/spectrotemporal", {"file_path": missing}),
        ("/analyze/signal_integrity", {"file_path": missing}),
        ("/analyze/pan_balance", {"file_path": missing}),
        ("/analyze/dither/preview", {"file_path": missing}),
        ("/analyze/stem_energy_timeline", {"vocals_path": missing}),
        ("/analyze/album", {"track_paths": [missing]}),
    ]:
        r = client.post(route, json=body)
        assert r.status_code == 404, (route, r.text)
        assert "not found" in r.json()["detail"]


def test_dependencies_declared():
    import tomllib
    from pathlib import Path
    data = tomllib.loads((Path(__file__).resolve().parents[1] / "pyproject.toml").read_text("utf-8"))
    base = " ".join(data["project"]["dependencies"])
    extras = data["project"]["optional-dependencies"]
    assert "pydantic" in base
    assert "python-rtmidi" not in base and any("python-rtmidi" in d for d in extras["midi"])
    assert any(d.startswith("huggingface_hub") for d in extras["ai_detect"])
    assert json.dumps(extras)   # sanity
