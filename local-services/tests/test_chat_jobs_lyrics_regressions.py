"""Chat / jobs / lyrics findings from the 2026-10-15 audit (slice P2).

AUDIT-043 chat history windowed server-side (never 422) + input-token budget,
AUDIT-072 dedup keys carry platforms and the file's mtime+size,
AUDIT-080 whisper VAD, word probabilities gate "masked", locked model cache,
AUDIT-081 local LLM connect/read timeouts, bad chain item dropped, stream closed.
"""
from __future__ import annotations

import threading
import time
import types

import numpy as np
import soundfile as sf

import khensu_services.copilot.router_chat as rc
from khensu_services.copilot.router_chat import ChatMessage, _window_history


def _history(n):
    return [{"role": "user" if i % 2 == 0 else "assistant", "content": f"message {i} " + "x" * 200}
            for i in range(n)]


# --- AUDIT-043 ---------------------------------------------------------------

def test_long_history_is_accepted_not_422(client, monkeypatch):
    monkeypatch.setattr(rc, "get_provider_for_purpose", lambda purpose: None)
    monkeypatch.setattr(rc, "query_local_llm", lambda *a, **k: None)
    r = client.post("/copilot/chat", json={"message": "more air", "history": _history(1500)})
    assert r.status_code == 200, r.text


def test_window_keeps_first_and_latest_turns_under_budget():
    msgs = [ChatMessage(**m) for m in _history(300)]
    kept = _window_history(msgs, budget_tokens=3000, reserved_tokens=1000)
    assert kept[0].content == msgs[0].content                # opening context
    assert kept[-1].content == msgs[-1].content              # newest turn
    assert len(kept) < len(msgs)
    assert sum(rc._estimate_tokens(m.content) for m in kept) <= 2000
    for a, b in zip(kept, kept[1:]):
        assert a.role != b.role                              # still alternates


def test_window_is_empty_when_the_prompt_alone_fills_the_budget():
    msgs = [ChatMessage(**m) for m in _history(10)]
    assert _window_history(msgs, budget_tokens=500, reserved_tokens=600) == []


class _StructuredProvider:
    name = "fake"

    def __init__(self, data, budget=2000):
        self.data = data
        self.budget = budget
        self.messages = None

    def model_name(self):
        return "fake-1"

    def max_input_tokens(self):
        return self.budget

    def generate_structured(self, **kw):
        self.messages = kw["messages"]
        return self.data


def test_provider_receives_a_windowed_history(client, monkeypatch):
    prov = _StructuredProvider({"response": "ok", "suggested_chain": None}, budget=6000)
    monkeypatch.setattr(rc, "get_provider_for_purpose", lambda purpose: prov)
    r = client.post("/copilot/chat", json={"message": "hi", "history": _history(400)})
    assert r.status_code == 200 and r.json()["source"] == "fake"
    assert 1 < len(prov.messages) < 400
    assert prov.messages[-1]["role"] == "user" and "hi" in prov.messages[-1]["content"]
    total = sum(rc._estimate_tokens(m["content"]) for m in prov.messages)
    assert total <= prov.budget


# --- AUDIT-081 ---------------------------------------------------------------

def test_one_bad_chain_item_is_dropped_not_the_reply(client, monkeypatch):
    prov = _StructuredProvider({"response": "Tame the lows.", "suggested_chain": [
        {"effect": "HighpassFilter", "parameters": {"cutoff_frequency_hz": 80.0}},
        {"parameters": {"ratio": 4.0}},                      # no effect name
        "not an object",
    ]}, budget=100_000)
    monkeypatch.setattr(rc, "get_provider_for_purpose", lambda purpose: prov)
    r = client.post("/copilot/chat", json={"message": "clean up the low end"})
    body = r.json()
    assert body["source"] == "fake" and body["response"] == "Tame the lows."
    assert [e["effect"] for e in body["suggested_chain"]] == ["HighpassFilter"]


def test_local_llm_timeout_splits_connect_and_read(monkeypatch):
    from khensu_services.copilot.llm_config import local_llm_timeout
    monkeypatch.delenv("KHENSU_LOCAL_LLM_TIMEOUT_SEC", raising=False)
    t = local_llm_timeout()
    assert t.connect == 5.0 and 60.0 <= t.read <= 120.0
    monkeypatch.setenv("KHENSU_LOCAL_LLM_TIMEOUT_SEC", "30")
    assert local_llm_timeout().read == 30.0


def test_blocking_stream_close_waits_for_inflight_read_then_closes():
    state = {"closed": False}

    def slow_gen():
        try:
            while True:
                time.sleep(0.2)
                yield "chunk"
        finally:
            state["closed"] = True

    from khensu_services.copilot import router_chat_stream
    stream = router_chat_stream._BlockingStream(slow_gen())
    sentinel = object()
    reader = threading.Thread(target=stream.next_chunk, args=(sentinel,))
    reader.start()
    time.sleep(0.05)                     # the read is now in flight
    stream.close()                       # must not fail with "already executing"
    reader.join()
    assert state["closed"] is True
    assert stream.next_chunk(sentinel) is sentinel


def test_stream_generator_is_closed_after_the_response(client, monkeypatch):
    import json
    state = {"closed": False}
    doc = json.dumps({"response": "ok", "suggested_chain": None})

    class _StreamProvider:
        name = "fake"

        def model_name(self):
            return "fake-1"

        def generate_structured_stream(self, **kw):
            try:
                yield doc
            finally:
                state["closed"] = True

    import khensu_services.copilot.router_chat_stream as rcs
    monkeypatch.setattr(rcs, "get_provider_for_purpose", lambda purpose: _StreamProvider())
    r = client.post("/copilot/chat/stream", json={"message": "x"})
    assert r.status_code == 200
    deadline = time.monotonic() + 2.0
    while not state["closed"] and time.monotonic() < deadline:
        time.sleep(0.02)
    assert state["closed"] is True


# --- AUDIT-072 ---------------------------------------------------------------

def _wav(path, secs=0.5):
    sf.write(str(path), np.zeros(int(44100 * secs), dtype=np.float32), 44100)
    return str(path)


def test_platform_masters_dedup_distinguishes_platform_sets(client, tmp_path, monkeypatch):
    import khensu_services.copilot.router_platform_masters as rpm
    release = threading.Event()
    monkeypatch.setattr(rpm, "_run_platform_job", lambda job_id, *a: release.wait(5.0))
    src = _wav(tmp_path / "m.wav")
    try:
        a = client.post("/copilot/platform_masters/start",
                        json={"file_path": src, "platforms": ["spotify"]}).json()["job_id"]
        b = client.post("/copilot/platform_masters/start",
                        json={"file_path": src, "platforms": ["apple"]}).json()["job_id"]
        c = client.post("/copilot/platform_masters/start",
                        json={"file_path": src, "platforms": ["spotify"]}).json()["job_id"]
        assert a != b
        assert a == c
    finally:
        release.set()


def test_analyze_dedup_key_tracks_file_content(client, tmp_path, monkeypatch):
    import khensu_services.analysis.router_audio as ra
    keys = []

    def capture(store, job_id, payload, dedup_key):
        keys.append(dedup_key)
        return job_id, False

    monkeypatch.setattr(ra, "admit_or_join_or_429", capture)
    monkeypatch.setattr(ra, "_analysis_cache_key", lambda *a: None)
    src = tmp_path / "a.wav"
    _wav(src, 0.5)
    client.post("/analyze/start", json={"file_path": str(src), "mode": "quick"})
    time.sleep(0.01)
    _wav(src, 0.75)                      # re-rendered in place
    client.post("/analyze/start", json={"file_path": str(src), "mode": "quick"})
    assert len(keys) == 2 and keys[0] != keys[1]


# --- AUDIT-080 ---------------------------------------------------------------

def test_transcription_uses_vad_and_keeps_word_probability(monkeypatch):
    from khensu_services.analysis import lyric_analyzer as la
    seen = {}

    class _Model:
        def transcribe(self, path, **kw):
            seen.update(kw)
            words = [types.SimpleNamespace(word=" hello", start=0.1, end=0.4, probability=0.91)]
            return [types.SimpleNamespace(words=words)], None

    monkeypatch.setattr(la, "_get_model", lambda size="base": _Model())
    words = la.transcribe_lyrics("x.wav")
    assert seen.get("vad_filter") is True and seen.get("word_timestamps") is True
    assert words == [{"word": "hello", "start_sec": 0.1, "end_sec": 0.4, "probability": 0.91}]


def test_low_confidence_words_are_never_flagged_masked():
    from khensu_services.analysis import lyric_analyzer as la
    sr = 16000
    t = np.arange(sr * 4) / sr
    y = 0.3 * np.sin(2 * np.pi * 3000 * t)             # presence-band rich...
    y[sr:2 * sr] = 0.3 * np.sin(2 * np.pi * 300 * t[sr:2 * sr])   # ...except 1-2 s
    words = [
        {"word": "real", "start_sec": 1.2, "end_sec": 1.6, "probability": 0.9},
        {"word": "ghost", "start_sec": 1.2, "end_sec": 1.6, "probability": 0.2},
    ]
    out = la.analyze_lyric_intelligibility(y.astype(np.float32), sr, words)
    flags = {w["word"]: w["likely_masked"] for w in out["words"]}
    assert flags == {"real": True, "ghost": False}
    assert out["flagged_count"] == 1


def test_model_cache_loads_once_under_concurrency(monkeypatch):
    from khensu_services.analysis import lyric_analyzer as la
    la._model_cache.clear()
    loads = []

    def slow_load(size):
        time.sleep(0.2)
        loads.append(size)
        model = object()
        la._model_cache[size] = model
        return model

    monkeypatch.setattr(la, "_load_model", slow_load)
    try:
        threads = [threading.Thread(target=la._get_model, args=("tiny",)) for _ in range(4)]
        for th in threads:
            th.start()
        for th in threads:
            th.join()
        assert loads == ["tiny"]
    finally:
        la._model_cache.clear()
