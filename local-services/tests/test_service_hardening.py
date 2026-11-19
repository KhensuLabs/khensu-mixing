"""Regression tests for the 2026-10-10 service audit fixes (render/jobs/chat/
transport/separation/model-dir hardening). Hermetic: no network, no real
LLM, renders are stubbed where the real DSP isn't the subject."""
from __future__ import annotations

import importlib
import json
import os
import threading
import time
from concurrent.futures import Future

import numpy as np
import pytest
import soundfile as sf


def _wav(path, seconds=0.5, sr=22050, channels=1):
    y = (0.1 * np.sin(2 * np.pi * 440 * np.arange(int(sr * seconds)) / sr)).astype(np.float32)
    if channels > 1:
        y = np.stack([y] * channels, axis=1)
    sf.write(str(path), y, sr)
    return str(path)


# ---------------------------------------------------------------- jobs (6, 7)
def test_cancelling_job_is_not_joinable():
    from khensu_services.common.jobs import JobStore
    store = JobStore(name="t", max_active=4)
    jid, created = store.admit_or_join("a", {"status": "running"}, "key")
    assert created
    assert store.find_active_by_dedup_key("key") == "a"
    store["a"]["status"] = "cancelling"
    assert store.find_active_by_dedup_key("key") is None
    jid2, created2 = store.admit_or_join("b", {"status": "pending"}, "key")
    assert (jid2, created2) == ("b", True)


def test_request_fingerprint_is_stable_and_parameter_sensitive():
    from khensu_services.common.jobs import request_fingerprint
    assert request_fingerprint({"a": 1, "b": 2}) == request_fingerprint({"b": 2, "a": 1})
    assert request_fingerprint({"a": 1}) != request_fingerprint({"a": 2})


def test_apply_chain_dedup_distinguishes_different_chains(client, tmp_path, monkeypatch):
    import khensu_services.copilot.router_render as rr
    submitted = []

    class _NoRun:
        def submit(self, *a, **k):
            submitted.append(a)
            from concurrent.futures import Future
            return Future()   # start_job tracks futures for cancel_reply

    monkeypatch.setattr(rr, "_executor", _NoRun())
    rr._apply_jobs.clear()
    src = _wav(tmp_path / "src.wav")
    out = str(tmp_path / "out.wav")
    body = {"file_path": src, "output_path": out,
            "effects": [{"effect": "Gain", "parameters": {"gain_db": -1.0}}]}
    a = client.post("/copilot/apply_chain/start", json=body).json()["job_id"]
    a2 = client.post("/copilot/apply_chain/start", json=body).json()["job_id"]
    body["effects"][0]["parameters"]["gain_db"] = -3.0
    b = client.post("/copilot/apply_chain/start", json=body).json()["job_id"]
    assert a == a2            # identical request joins
    assert a != b             # different chain -> separate job
    assert len(submitted) == 2
    rr._apply_jobs.clear()


# ------------------------------------------------------- BoundedCache thread-safety
def test_bounded_cache_concurrent_access_keeps_invariants():
    from khensu_services.common.lru import BoundedCache
    cache = BoundedCache(max_entries=8)
    errors = []

    def worker(n):
        try:
            for i in range(400):
                cache[(n, i % 20)] = {"v": i}
                cache.get((n, (i * 7) % 20))
                cache.pop((n, (i * 3) % 20), None)
        except Exception as exc:  # pragma: no cover - failure path
            errors.append(exc)

    threads = [threading.Thread(target=worker, args=(n,)) for n in range(6)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    assert not errors
    assert len(cache) <= 8
    assert set(cache._sizes) == set(cache.keys())
    assert cache.total_bytes == sum(cache._sizes.values())


# ------------------------------------------------------------- batch render (2, 8)
@pytest.mark.parametrize("suffix", ["../evil", "a/b", "a\\b", "C:x", "bad\x00", "x\ny"])
def test_batch_rejects_unsafe_suffix(client, tmp_path, suffix):
    src = _wav(tmp_path / "a.wav")
    r = client.post("/copilot/apply_chain/batch/start",
                    json={"file_paths": [src], "suffix": suffix})
    assert r.status_code == 422


def test_batch_job_failure_outside_per_file_loop_marks_failed(tmp_path, monkeypatch):
    import khensu_services.copilot.router_render_batch as rr
    req = rr.ApplyChainBatchRequest(file_paths=[])
    rr._batch_jobs["jb"] = {"status": "pending", "progress": {"total": 1, "done": 0, "failed": 0},
                            "results": [], "error": None}

    def boom(*a, **k):
        raise OSError("disk gone")

    monkeypatch.setattr(rr.os, "makedirs", boom)
    rr._run_batch_job("jb", req, ["x.wav"], str(tmp_path / "out"))
    assert rr._batch_jobs["jb"]["status"] == "failed"
    assert "disk gone" in rr._batch_jobs["jb"]["error"]


def test_batch_job_does_not_overwrite_interrupted(tmp_path, monkeypatch):
    import khensu_services.copilot.router_render_batch as rr
    req = rr.ApplyChainBatchRequest(file_paths=[])
    job = {"status": "pending", "progress": {"total": 1, "done": 0, "failed": 0},
           "results": [], "error": None}
    rr._batch_jobs["ji"] = job

    def fake_chain(src, out, effects, normalize=False):
        job["status"] = "interrupted"   # service shutdown mid-render
        return {"applied": [], "skipped": []}

    monkeypatch.setattr(importlib.import_module("khensu_services.copilot.router_render"), "apply_pedalboard_chain", fake_chain)
    src = _wav(tmp_path / "a.wav")
    rr._run_batch_job("ji", req, [src], str(tmp_path / "out"))
    assert job["status"] == "interrupted"


def test_batch_validates_each_output_path(tmp_path, monkeypatch):
    import khensu_services.copilot.router_render_batch as rr
    from fastapi import HTTPException
    req = rr.ApplyChainBatchRequest(file_paths=[])
    job = {"status": "pending", "progress": {"total": 1, "done": 0, "failed": 0},
           "results": [], "error": None}
    rr._batch_jobs["jv"] = job
    seen = []

    def fake_validate(out_path, src=None):
        seen.append(out_path)
        raise HTTPException(status_code=403, detail="outside allowed roots")

    import khensu_services.common.output_paths as op
    monkeypatch.setattr(op, "validate_output_path", fake_validate)
    rendered = []
    monkeypatch.setattr(importlib.import_module("khensu_services.copilot.router_render"), "apply_pedalboard_chain", lambda *a, **k: rendered.append(a))
    src = _wav(tmp_path / "a.wav")
    rr._run_batch_job("jv", req, [src], str(tmp_path / "out"))
    assert seen and not rendered
    assert job["results"][0]["status"] == "failed"
    assert job["status"] == "completed"


# ------------------------------------------------------------ override guardrail (3)
def test_override_rejects_quarantine_from_other_directory(client, tmp_path):
    import khensu_services.copilot.router_render as rr
    src = _wav(tmp_path / "src.wav")
    out = str(tmp_path / "out" / "mix.wav")
    os.makedirs(tmp_path / "elsewhere")
    os.makedirs(tmp_path / "out")
    # Same basename as the real quarantine path, different directory.
    fake_q = tmp_path / "elsewhere" / os.path.basename(rr._quarantine_path_for(out))
    _wav(fake_q)
    r = client.post("/copilot/override_guardrail", json={
        "source_path": src, "quarantine_path": str(fake_q), "output_path": out,
        "expert_note": "ok"})
    assert r.status_code == 422


def test_override_rejects_output_aliasing_source(client, tmp_path):
    import khensu_services.copilot.router_render as rr
    src = _wav(tmp_path / "src.wav")
    q = rr._quarantine_path_for(src)
    _wav(q)
    r = client.post("/copilot/override_guardrail", json={
        "source_path": src, "quarantine_path": q, "output_path": src, "expert_note": "ok"})
    assert r.status_code == 409
    assert os.path.isfile(q)        # nothing promoted


def test_same_path_is_case_insensitive_where_the_fs_is():
    import khensu_services.copilot.router_render as rr
    assert rr._same_path("a/b/../c.wav", "a/c.wav")
    if os.name == "nt":
        assert rr._same_path("C:/X/Y.wav", "c:\\x\\y.wav")


# ------------------------------------------------------------ cancelled apply job (5)
def test_cancelled_apply_job_discards_quarantine(tmp_path, monkeypatch):
    import khensu_services.copilot.router_render as rr
    import khensu_services.copilot.render_validator as rv
    monkeypatch.setattr(rv, "validate_stem_attribution", lambda *a: {"ok": True})
    monkeypatch.setattr(rv, "validate_render_request", lambda *a: {"ok": True})
    logged = []
    monkeypatch.setattr(rr, "_log_applied_event", lambda *a, **k: logged.append(a))
    src = _wav(tmp_path / "src.wav")
    out = str(tmp_path / "out.wav")
    req = rr.ApplyChainRequest(file_path=src, output_path=out,
                               effects=[{"effect": "Gain", "parameters": {}}], session_id="s")
    rr._apply_jobs["jc"] = {"status": "pending", "result": None, "error": None}

    def fake_render(src_, dst, effects, normalize=False):
        _wav(dst)
        rr._apply_jobs["jc"]["status"] = "cancelled"   # user cancelled mid-render
        return {"applied": ["Gain"], "skipped": []}

    monkeypatch.setattr(importlib.import_module("khensu_services.copilot.router_render"), "apply_pedalboard_chain", fake_render)
    rr._run_apply_job("jc", req)
    assert rr._apply_jobs["jc"]["status"] == "cancelled"
    assert not os.path.exists(out)
    assert not os.path.exists(rr._quarantine_path_for(out))
    assert logged == []


# ------------------------------------------------------------ full intake (8)
def test_intake_retry_missing_file_is_404(client, tmp_path):
    r = client.post("/analyze/full_intake/retry/quick_stats",
                    json={"file_path": str(tmp_path / "missing.wav")})
    assert r.status_code == 404


async def test_intake_job_keeps_interrupted(monkeypatch):
    import khensu_services.analysis.router_full_intake as fi
    from khensu_services.analysis.models import FullIntakeRequest
    job = {"state": "queued", "stage": None, "stage_state": "queued", "cancelled": False}
    fi._intake_jobs["ij"] = job

    def fake_run_intake(*a, **k):
        job["state"] = "interrupted"
        return {"ok": True}

    # The job worker now runs _run_intake directly on the long-job pool.
    monkeypatch.setattr(fi, "_run_intake", fake_run_intake)
    await fi._run_intake_job("ij", FullIntakeRequest(file_path="x.wav"))
    assert job["state"] == "interrupted"
    assert "result" not in job


# ------------------------------------------------------------ artifacts cache key (9)
def test_artifacts_cache_key_tracks_stem_content(tmp_path):
    from khensu_services.analysis.router_artifacts_scan import _cache_key
    main = _wav(tmp_path / "m.wav")
    stem = _wav(tmp_path / "v.wav")
    k1 = _cache_key(main, {"vocals": stem})
    time.sleep(0.01)
    _wav(stem, seconds=0.75)          # re-rendered in place: same path
    k2 = _cache_key(main, {"vocals": stem})
    assert k1 != k2


# ------------------------------------------------------------ decode budget (10)
def test_decode_budget_guard_blocks_oversized_analysis_input(client, tmp_path, monkeypatch):
    import khensu_services.common.audio_limits as al
    src = _wav(tmp_path / "long.wav", seconds=1.0)
    monkeypatch.setattr(al, "_MAX_AUDIO_DURATION_SECONDS", 0.5)
    r = client.post("/analyze/quick_stats", json={"file_path": src})
    assert r.status_code == 413
    r = client.post("/report/compliance", json={"file_path": src})
    assert r.status_code == 413


def test_decode_budget_guard_collects_nested_inputs_only(tmp_path):
    from khensu_services.common.audio_limits import collect_audio_input_paths
    a = _wav(tmp_path / "a.wav")
    b = _wav(tmp_path / "b.wav")
    c = _wav(tmp_path / "c.wav")
    body = {"file_path": a, "stems": {"vocals": b}, "output_path": c,
            "tracks": [{"reference_path": c}], "file_paths": [str(tmp_path / "nope.wav")]}
    found = collect_audio_input_paths(body)
    assert set(found) == {a, b, c}    # c via nested reference_path, not output_path


def test_decode_budget_guard_rejects_unreadable_files():
    # Bad-input sweep 2026-10-16: left to the endpoints, an unreadable file
    # became a 500 on ~40 routes, so the guard now rejects it (-> 422).
    from khensu_services.common.audio_limits import AudioInputProbeError, check_audio_inputs
    import tempfile
    with tempfile.NamedTemporaryFile(suffix=".wav", delete=False) as f:
        f.write(b"not audio")
    try:
        with pytest.raises(AudioInputProbeError):
            check_audio_inputs([f.name])
    finally:
        os.unlink(f.name)


# ------------------------------------------------------------ chat stream (4, 12)
def test_response_extractor_unescapes_and_handles_split_escapes():
    from khensu_services.copilot.router_chat_stream import _ResponseFieldExtractor
    doc = json.dumps({"response": 'He said "hi"\nnext line \u00e9 \U0001F600 end', "target_stem": "mix"})
    ex = _ResponseFieldExtractor()
    out = "".join(ex.feed(doc[i:i + 3]) for i in range(0, len(doc), 3))
    assert out == 'He said "hi"\nnext line \u00e9 \U0001F600 end'


class _FakeProvider:
    name = "fake"

    def __init__(self, chunks, fail_after=None):
        self._chunks = chunks
        self._fail_after = fail_after
        self.loop_threads = []

    def model_name(self):
        return "fake-1"

    def generate_structured_stream(self, **kwargs):
        import asyncio
        for i, c in enumerate(self._chunks):
            try:
                asyncio.get_running_loop()
                self.loop_threads.append(True)    # iterated ON an event loop: bad
            except RuntimeError:
                self.loop_threads.append(False)
            if self._fail_after is not None and i >= self._fail_after:
                raise RuntimeError("connection reset")
            yield c


def _sse(r):
    return [json.loads(line[6:]) for line in r.iter_lines() if line.startswith("data: ")]


def test_chat_stream_runs_provider_off_loop(client, monkeypatch):
    import khensu_services.copilot.router_chat_stream as rc
    doc = json.dumps({"response": "Cut 300 Hz \"mud\".", "target_stem": "mix", "suggested_chain": []})
    prov = _FakeProvider([doc[i:i + 5] for i in range(0, len(doc), 5)])
    monkeypatch.setattr(rc, "get_provider_for_purpose", lambda purpose: prov)
    r = client.post("/copilot/chat/stream", json={"message": "x", "history": []})
    events = _sse(r)
    tokens = "".join(e["content"] for e in events if e["type"] == "token")
    assert tokens == 'Cut 300 Hz "mud".'
    assert events[-1]["type"] == "final"
    assert events[-1]["result"]["source"] == "fake"
    assert prov.loop_threads and not any(prov.loop_threads)


def test_chat_stream_partial_failure_does_not_append_heuristics(client, monkeypatch):
    import khensu_services.copilot.router_chat_stream as rc
    chunks = ['{"response": "Partial ', 'answer', ' more']
    prov = _FakeProvider(chunks, fail_after=2)
    monkeypatch.setattr(rc, "get_provider_for_purpose", lambda purpose: prov)
    r = client.post("/copilot/chat/stream", json={"message": "make it dreamy", "history": []})
    events = _sse(r)
    tokens = "".join(e["content"] for e in events if e["type"] == "token")
    assert tokens == "Partial answer"
    assert any(e["type"] == "error" for e in events)
    assert events[-1]["type"] == "final"
    assert events[-1]["result"]["response"] == "Partial answer"


def test_chat_stream_admission_cap(client, monkeypatch):
    import khensu_services.copilot.router_chat_stream as rc
    rc._chat_streams.clear()
    for i in range(rc._chat_streams._max_active):
        rc._chat_streams[f"busy{i}"] = {"status": "running"}
    try:
        r = client.post("/copilot/chat/stream", json={"message": "x", "history": []})
        assert r.status_code == 429
    finally:
        rc._chat_streams.clear()


# ------------------------------------------------------------ LLM response cache (13)
def test_llm_cache_skips_unparseable_and_dedups_order(monkeypatch):
    import khensu_services.copilot.llm_provider as lp
    monkeypatch.setattr(lp, "_response_cache", {})
    monkeypatch.setattr(lp, "_cache_order", [])

    class Inner:
        name = "inner"
        calls = 0

        def model_name(self):
            return "m"

        def is_available(self):
            return True

        def generate_structured(self, **kw):
            Inner.calls += 1
            return {}

        def generate_structured_stream(self, **kw):
            yield "not json at all"

    w = lp._CachingProviderWrapper(Inner())
    kw = dict(system="s", messages=[], max_tokens=1, tool_name="t", tool_description="d", input_schema={})
    w.generate_structured(**kw)
    w.generate_structured(**kw)
    assert Inner.calls == 2                       # empty result never cached
    list(w.generate_structured_stream(**kw))
    assert lp._response_cache == {}               # unparseable stream never cached

    lp._cache_put("k", {"a": 1})
    monkeypatch.setattr(lp, "_CACHE_TTL_SEC", -1)
    assert lp._cache_get("k") is None             # expired
    lp._cache_put("k", {"a": 2})
    assert lp._cache_order.count("k") == 1


# ------------------------------------------------------------ transport (14)
def test_transport_never_uses_arbitrary_port(monkeypatch):
    import khensu_services.analysis.router_transport as rt
    opened = []

    def open_output(name=None, virtual=False):
        if virtual:
            raise NotImplementedError
        opened.append(name)
        return object()

    monkeypatch.setattr(rt, "_midi_out_port", None)
    monkeypatch.delenv("KHENSU_MIDI_OUT_PORT", raising=False)
    monkeypatch.setattr(rt.mido, "open_output", open_output)
    monkeypatch.setattr(rt.mido, "get_output_names", lambda: ["Roland Synth", "Microsoft GS Wavetable"])
    assert rt.get_midi_out() is None
    assert opened == []
    monkeypatch.setattr(rt.mido, "get_output_names", lambda: ["Roland Synth", "loopMIDI Khensu Port 1"])
    assert rt.get_midi_out() is not None
    assert opened == ["loopMIDI Khensu Port 1"]
    monkeypatch.setattr(rt, "_midi_out_port", None)


@pytest.mark.parametrize("body", ['{"position_sec": "abc"}', '[1, 2]', 'not json',
                                  '{"is_playing": "yes"}'])
def test_transport_playhead_invalid_body_is_422(client, body):
    r = client.post("/transport/playhead", content=body,
                    headers={"content-type": "application/json"})
    assert r.status_code == 422


def test_transport_params_invalid_body_is_422(client):
    r = client.post("/transport/params", json={"params": [1, 2]})
    assert r.status_code == 422


# ------------------------------------------------------------ separation (15, 16)
def test_separate_future_dropped_after_successful_cancel():
    import khensu_services.analysis.router_separate as rs
    fut: Future = Future()
    rs._jobs["sf"] = {"status": "pending", "result": None, "error": None}
    rs._track_future("sf", fut)
    assert rs._job_futures.get("sf") is fut
    import asyncio
    asyncio.run(rs.separate_cancel("sf"))
    assert rs._jobs["sf"]["status"] == "cancelled"
    assert "sf" not in rs._job_futures


def test_separate_track_future_already_done_is_not_leaked():
    import khensu_services.analysis.router_separate as rs
    fut: Future = Future()
    fut.set_result(None)            # worker finished before registration
    rs._track_future("sd", fut)
    assert "sd" not in rs._job_futures


def test_separate_dedup_includes_output_dir(client, tmp_path, monkeypatch):
    import khensu_services.analysis.router_separate as rs
    submitted = []

    class _NoRun:
        def submit(self, *a, **k):
            submitted.append(a)
            return Future()

    monkeypatch.setattr(rs, "_separation_executor", _NoRun())
    rs._jobs.clear()
    src = _wav(tmp_path / "s.wav")
    a = client.post("/separate/start", json={"file_path": src, "output_dir": str(tmp_path / "o1")})
    b = client.post("/separate/start", json={"file_path": src, "output_dir": str(tmp_path / "o2")})
    assert a.status_code == b.status_code == 200
    assert a.json()["job_id"] != b.json()["job_id"]
    rs._jobs.clear()
    rs._job_futures.clear()


# ------------------------------------------------------------ models dir (17)
def test_set_models_dir_validation(client, tmp_path, monkeypatch):
    import khensu_services.copilot.downloader as dl
    saved = []
    monkeypatch.setattr(dl.ModelDownloader, "save_config", lambda self, d: saved.append(d))
    assert client.post("/copilot/set_models_dir", json={"models_dir": "relative/dir"}).status_code == 422
    assert client.post("/copilot/set_models_dir", json={"models_dir": "\\\\server\\share\\m"}).status_code == 403  # local_path_guard (AUDIT-063)
    assert client.post("/copilot/set_models_dir", json={"models_dir": "//server/share/m"}).status_code == 403  # local_path_guard (AUDIT-063)
    f = tmp_path / "file.txt"
    f.write_text("x")
    assert client.post("/copilot/set_models_dir", json={"models_dir": str(f)}).status_code == 422
    good = tmp_path / "models"
    r = client.post("/copilot/set_models_dir", json={"models_dir": str(good)})
    assert r.status_code == 200 and good.is_dir() and saved


def test_set_models_dir_save_failure_is_500(client, tmp_path, monkeypatch):
    import khensu_services.copilot.downloader as dl

    def boom(self, d):
        raise PermissionError("read-only config dir")

    monkeypatch.setattr(dl.ModelDownloader, "save_config", boom)
    r = client.post("/copilot/set_models_dir", json={"models_dir": str(tmp_path / "m")})
    assert r.status_code == 500


# ------------------------------------------------------------ mastering agent dedup (6)
def test_mastering_agent_dedup_includes_parameters(client, tmp_path, monkeypatch):
    import khensu_services.copilot.router_mastering_agent as ma
    import khensu_services.copilot.render_validator as rv
    monkeypatch.setattr(rv, "validate_stem_attribution", lambda *a: {"ok": True})

    class _NoRun:
        def submit(self, *a, **k):
            from concurrent.futures import Future
            return Future()   # start_job tracks futures for cancel_reply

    monkeypatch.setattr(ma, "_executor", _NoRun())
    ma._mastering_jobs.clear()
    src = _wav(tmp_path / "m.wav")
    a = client.post("/copilot/mastering_agent/start", json={"file_path": src, "max_iterations": 2}).json()
    b = client.post("/copilot/mastering_agent/start", json={"file_path": src, "max_iterations": 3}).json()
    assert a["job_id"] != b["job_id"]
    ma._mastering_jobs.clear()
