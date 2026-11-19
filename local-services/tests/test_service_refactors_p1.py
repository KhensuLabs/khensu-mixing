"""Regression tests for the service-logic / DSP-math refactor slice:

* LLM work runs on its own bounded pool (never the 4-worker DSP pool)
* shared job start/status/cancel helpers in common/jobs.py
* no router imports khensu_services.server (the old circular import)
* status-code convention (common/errors.py)
* /recommend removed
* cwd is not an allowed output root
* crest factor measured per channel (max), not on the mono downmix
"""
from __future__ import annotations

import importlib

import subprocess
import sys
import threading
from concurrent.futures import Future, ThreadPoolExecutor
from pathlib import Path

import numpy as np
import pytest
import soundfile as sf
from fastapi import HTTPException

_SERVICE_ROOT = Path(__file__).resolve().parent.parent


def _wav(path, seconds=0.5, sr=22050):
    y = (0.1 * np.sin(2 * np.pi * 440 * np.arange(int(sr * seconds)) / sr)).astype(np.float32)
    sf.write(str(path), y, sr)
    return str(path)


# --------------------------------------------------------------- LLM pool (1)
def test_llm_pool_is_separate_from_dsp_pool():
    from khensu_services.common import executors as ex
    assert ex.llm_executor is not ex.dsp_executor
    assert ex.llm_executor is not ex.separation_executor


async def test_run_llm_blocking_uses_llm_threads():
    from khensu_services.common.executors import run_blocking, run_llm_blocking
    llm_thread = await run_llm_blocking(lambda: threading.current_thread().name)
    dsp_thread = await run_blocking(lambda: threading.current_thread().name)
    assert llm_thread.startswith("khensu-llm")
    assert dsp_thread.startswith("khensu-worker")


async def test_llm_call_not_starved_by_saturated_dsp_pool():
    """Every DSP worker blocked must not delay an LLM-pool call."""
    import asyncio

    from khensu_services.common import executors as ex
    release = threading.Event()
    blockers = [ex.dsp_executor.submit(release.wait, 5) for _ in range(ex.dsp_executor._max_workers)]
    try:
        result = await asyncio.wait_for(ex.run_llm_blocking(lambda: "ok"), timeout=2.0)
        assert result == "ok"
    finally:
        release.set()
        for f in blockers:
            f.result(timeout=5)


def test_chat_and_mixer_chain_jobs_submit_to_llm_pool(client, monkeypatch):
    import khensu_services.copilot.router_chat as rc
    import khensu_services.copilot.router_render_sections as rr

    class _Recorder:
        def __init__(self):
            self.calls = []

        def submit(self, fn, *a, **k):
            self.calls.append(fn)
            return Future()

    llm, dsp = _Recorder(), _Recorder()
    monkeypatch.setattr(rc, "_llm_executor", llm)
    monkeypatch.setattr(rr, "_llm_executor", llm)
    monkeypatch.setattr(importlib.import_module("khensu_services.copilot.router_render"), "_executor", dsp)
    rc._chat_jobs.clear()
    rr._mixer_chain_jobs.clear()
    assert client.post("/copilot/chat/start", json={"message": "hi"}).status_code == 200
    assert client.post("/copilot/generate_mixer_chain/start",
                       json={"target_channel": "vocals", "genre": "pop", "energy": "high",
                             "mood": "bright", "bpm": 120.0, "style_description": "",
                             "enabled_ecosystems": []}).status_code == 200
    assert llm.calls == [rc._run_chat_job, rr._run_mixer_chain_job]
    assert dsp.calls == []
    rc._chat_jobs.clear()
    rr._mixer_chain_jobs.clear()


# ------------------------------------------------------------ job helpers (2)
def test_start_job_admits_submits_and_dedups():
    from khensu_services.common.jobs import JobStore, start_job
    store = JobStore(name="t_start", max_active=4)
    pool = ThreadPoolExecutor(max_workers=1)
    seen = []
    gate = threading.Event()

    def worker(job_id, x):
        gate.wait(5)
        seen.append((job_id, x))
        store[job_id]["status"] = "completed"

    try:
        a, created_a = start_job(store, pool, worker, 1, payload={"status": "pending"}, dedup_key="k")
        b, created_b = start_job(store, pool, worker, 1, payload={"status": "pending"}, dedup_key="k")
        assert created_a and not created_b and a == b
        gate.set()
        pool.shutdown(wait=True)
        assert seen == [(a, 1)]
    finally:
        gate.set()


def test_start_job_429_at_cap():
    from khensu_services.common.jobs import JobStore, start_job

    class _Pool:
        def submit(self, *a, **k):
            return Future()

    store = JobStore(name="t_cap", max_active=1)
    start_job(store, _Pool(), lambda jid: None, payload={"status": "pending"})
    with pytest.raises(HTTPException) as exc:
        start_job(store, _Pool(), lambda jid: None, payload={"status": "pending"})
    assert exc.value.status_code == 429


def test_get_job_or_404():
    from khensu_services.common.jobs import JobStore, get_job_or_404
    store = JobStore(name="t_get")
    store["a"] = {"status": "pending"}
    assert get_job_or_404(store, "a") == {"status": "pending"}
    with pytest.raises(HTTPException) as exc:
        get_job_or_404(store, "missing", detail="Unknown intake job")
    assert exc.value.status_code == 404 and exc.value.detail == "Unknown intake job"


def test_cancel_job_cooperative_and_future_aware():
    from khensu_services.common.jobs import JobStore, cancel_job, track_future
    store = JobStore(name="t_cancel")
    store["plain"] = {"status": "running"}
    # No futures: "cancelling" until the worker acknowledges (audit M4).
    assert cancel_job(store, "plain") == "cancelling"
    store["done"] = {"status": "completed"}
    assert cancel_job(store, "done") == "completed"      # terminal: untouched
    assert cancel_job(store, "nope") == "unknown"

    futures: dict[str, Future] = {}
    store["queued"] = {"status": "pending"}
    track_future(futures, "queued", Future())             # not started -> cancellable
    assert cancel_job(store, "queued", futures=futures) == "cancelled"
    assert "queued" not in futures

    running = Future()
    running.set_running_or_notify_cancel()                # started -> can't cancel
    store["busy"] = {"status": "running"}
    track_future(futures, "busy", running)
    assert cancel_job(store, "busy", futures=futures) == "cancelling"
    running.set_result(None)
    assert "busy" not in futures                          # done-callback forgot it


def test_job_status_and_cancel_endpoints_keep_shapes(client):
    for status_path, cancel_path in [
        ("/copilot/mastering_agent/status/x", "/copilot/mastering_agent/x/cancel"),
        ("/copilot/platform_masters/status/x", "/copilot/platform_masters/x/cancel"),
        ("/copilot/apply_chain/status/x", "/copilot/apply_chain/x/cancel"),
        ("/copilot/apply_chain/batch/status/x", "/copilot/apply_chain/batch/x/cancel"),
        ("/copilot/apply_section_chain/status/x", "/copilot/apply_section_chain/x/cancel"),
        ("/copilot/chat/status/x", "/copilot/chat/x/cancel"),
    ]:
        r = client.get(status_path)
        assert r.status_code == 404 and r.json()["detail"] == "Job not found", status_path
        r = client.post(cancel_path)
        # SVC-14: ok only for a known job, plus the real status.
        assert r.status_code == 200 and r.json() == {"ok": False, "status": "unknown"}, cancel_path
    assert client.post("/analyze/x/cancel").json() == {"ok": False, "status": "unknown"}
    # /separate cancel now uses common.jobs.make_job_routes (SVC-14 shape).
    assert client.post("/separate/x/cancel").json() == {"ok": False, "status": "unknown"}
    assert client.get("/analyze/full_intake/status/x").json()["detail"] == "Unknown intake job"


# ---------------------------------------------------------- import order (3)
def test_routers_import_without_server_module():
    """Routers take their executors from common/, so importing one never
    drags in (or cycles back through) khensu_services.server."""
    code = (
        "import sys\n"
        "import khensu_services.analysis.router\n"
        "import khensu_services.copilot.router\n"
        "import khensu_services.report.router\n"
        "assert 'khensu_services.server' not in sys.modules, 'router imported server'\n"
    )
    proc = subprocess.run([sys.executable, "-c", code], cwd=str(_SERVICE_ROOT),
                          capture_output=True, text=True, timeout=300)
    assert proc.returncode == 0, proc.stderr[-2000:]


def test_no_service_module_imports_server():
    pkg = _SERVICE_ROOT / "khensu_services"
    offenders = [
        str(p.relative_to(pkg)) for p in pkg.rglob("*.py")
        if p.name != "server.py"
        and "from khensu_services.server import" in p.read_text(encoding="utf-8", errors="replace")
    ]
    assert offenders == []


# ----------------------------------------------------------- status codes (4)
def test_analyze_missing_file_is_404(client):
    for path in ("/analyze", "/analyze/start"):
        r = client.post(path, json={"file_path": "C:/does/not/exist.wav", "mode": "quick"})
        assert r.status_code == 404, path


def test_start_download_blocked_by_settings_is_403(client, monkeypatch):
    import khensu_services.common.online as online
    import khensu_services.copilot.downloader as dl
    monkeypatch.setattr(online, "online_allowed", lambda feature=None: False)
    monkeypatch.setattr(dl.ModelDownloader, "start",
                        lambda self: (setattr(self, "status", "blocked"),
                                      setattr(self, "error_message", "Model downloads are disabled in Settings")))
    r = client.post("/copilot/start_download")
    assert r.status_code == 403
    assert "disabled" in r.json()["detail"]


def test_error_helpers_map_to_convention():
    from khensu_services.common import errors
    cases = [(errors.not_found, 404), (errors.invalid_input, 422), (errors.too_large, 413),
             (errors.online_disabled, 403), (errors.conflict, 409), (errors.internal_error, 500)]
    for fn, code in cases:
        with pytest.raises(HTTPException) as exc:
            fn("x")
        assert exc.value.status_code == code
    with pytest.raises(HTTPException) as exc:
        errors.require_file("C:/does/not/exist.wav")
    assert exc.value.status_code == 404


# --------------------------------------------------------- /recommend gone (5)
def test_recommend_endpoint_removed(client):
    assert client.post("/recommend", json={"findings": []}).status_code == 404
    routes = client.get("/health").json()["routes"]
    assert "POST /recommend" not in routes


# ---------------------------------------------------------- output roots (6)
def test_cwd_is_not_an_allowed_output_root(tmp_path, monkeypatch):
    from khensu_services.common import output_paths as op
    # Put cwd somewhere that is under none of the other roots.
    fake_home = tmp_path / "home"
    fake_tmp = tmp_path / "tmp"
    fake_cfg = tmp_path / "cfg"
    cwd = tmp_path / "service_cwd"
    for d in (fake_home, fake_tmp, fake_cfg, cwd):
        d.mkdir()
    monkeypatch.setattr(op.Path, "home", classmethod(lambda cls: fake_home))
    monkeypatch.setattr(op.tempfile, "gettempdir", lambda: str(fake_tmp))
    import khensu_services.common.config as cfg
    monkeypatch.setattr(cfg, "config_dir", lambda: fake_cfg)
    monkeypatch.chdir(cwd)

    assert cwd.resolve() not in op.allowed_output_roots()
    with pytest.raises(HTTPException) as exc:
        op.validate_output_path(str(cwd / "out.wav"))
    assert exc.value.status_code == 403
    # Legitimate roots still pass.
    for root in (fake_home, fake_tmp):
        assert op.validate_output_path(str(root / "x" / "out.wav")) == (root / "x" / "out.wav").resolve()
    # The config dir (token.txt, settings.json) is no longer a root (AUDIT-023).
    with pytest.raises(HTTPException) as exc:
        op.validate_output_path(str(fake_cfg / "x" / "out.wav"))
    assert exc.value.status_code == 403
    src = tmp_path / "audio" / "song.wav"
    src.parent.mkdir()
    assert op.validate_output_path(str(src.parent / "render.wav"), str(src))


# ----------------------------------------------------------- crest factor (7)
def test_crest_factor_is_per_channel_max():
    from khensu_services.analysis.loudness import crest_factor_db
    sr = 48000
    t = np.arange(sr) / sr
    sine = 0.5 * np.sin(2 * np.pi * 1000 * t)          # crest 3.01 dB
    square = 0.5 * np.sign(np.sin(2 * np.pi * 50 * t))  # crest 0 dB
    stereo = np.stack([sine, square])
    assert crest_factor_db(stereo) == pytest.approx(20 * np.log10(np.sqrt(2)), abs=0.01)
    # Mono / identical channels: unchanged from the single-channel value.
    assert crest_factor_db(sine) == pytest.approx(crest_factor_db(np.stack([sine, sine])))


def test_crest_factor_ignores_antiphase_cancellation_and_silent_channel():
    from khensu_services.analysis.loudness import crest_factor_db
    rng = np.random.default_rng(0)
    x = rng.standard_normal(48000) * 0.1
    antiphase = np.stack([x, -x])
    # The mono downmix is digital silence (-> None); per channel it is the
    # real crest of the noise.
    assert crest_factor_db(antiphase.mean(axis=0)) is None
    assert crest_factor_db(antiphase) == pytest.approx(crest_factor_db(x))
    # One silent channel is skipped, not averaged in.
    one_sided = np.stack([x, np.zeros_like(x)])
    assert crest_factor_db(one_sided) == pytest.approx(crest_factor_db(x))
    assert crest_factor_db(np.zeros((2, 1000))) is None
    assert crest_factor_db(np.zeros((2, 0))) is None


def test_windowed_crest_curve_is_per_channel():
    from khensu_services.analysis.loudness import _windowed_crest_curve, crest_factor_db
    rng = np.random.default_rng(1)
    sr = 8000
    x = rng.standard_normal(sr * 4) * 0.1
    curve = _windowed_crest_curve(np.stack([x, -x]), sr, 1.0, 1.0)
    assert len(curve) == 4 and all(c is not None for c in curve)
    assert curve[0] == pytest.approx(crest_factor_db(x[:sr]))


def test_quick_stats_crest_is_per_channel(tmp_path):
    from khensu_services.analysis.quick_stats import analyze_quick_stats
    rng = np.random.default_rng(2)
    x = (rng.standard_normal(22050) * 0.1).astype(np.float32)
    path = tmp_path / "antiphase.wav"
    sf.write(str(path), np.stack([x, -x], axis=1), 22050, subtype="FLOAT")
    stats = analyze_quick_stats(str(path))
    from khensu_services.analysis.loudness import crest_factor_db
    assert stats["crest_factor_db"] == pytest.approx(round(crest_factor_db(x), 2), abs=0.02)
