"""FIX-17 (review 2026-11-09): no disk-cache I/O on the event loop, and the
lyric scan - whose result depends on the installed Whisper weights, which the
cache key cannot see - stays in memory only."""
from __future__ import annotations

import ast
import asyncio
from pathlib import Path

import numpy as np
import soundfile as sf

from khensu_services.common import analysis_cache as ac

_PKG = Path(ac.__file__).resolve().parents[1]

#: Cache calls that touch the disk (hashing a file, reading or writing an entry).
_DISK_IO = {"lookup", "store", "key_for", "key_for_analyzer", "content_hash", "make_key",
            "cached_call", "persistent_call", "stats", "clear"}
_CACHE_ALIASES = {"_acache", "analysis_cache"}


def _wav(path: Path, seconds: float = 3.0, sr: int = 22050) -> Path:
    t = np.arange(int(seconds * sr)) / sr
    mono = 0.3 * np.sin(2 * np.pi * 220.0 * t)
    sf.write(str(path), np.stack([mono, 0.9 * mono]).T.astype(np.float32), sr, subtype="FLOAT")
    return path


def _on_event_loop() -> bool:
    try:
        asyncio.get_running_loop()
    except RuntimeError:
        return False
    return True


# --------------------------------------------------------------------------- #
# chords
# --------------------------------------------------------------------------- #

def test_chords_cache_io_runs_off_the_event_loop(client, tmp_path, monkeypatch):
    from khensu_services.analysis import router_audio as ra

    seen: list[tuple[str, bool]] = []
    real_lookup, real_store = ac.lookup, ac.store

    def lookup(analyzer, key):
        seen.append(("lookup", _on_event_loop()))
        return real_lookup(analyzer, key)

    def store(analyzer, key, value):
        seen.append(("store", _on_event_loop()))
        return real_store(analyzer, key, value)

    monkeypatch.setattr(ac, "lookup", lookup)
    monkeypatch.setattr(ac, "store", store)

    a = str(_wav(tmp_path / "a.wav"))
    r1 = client.post("/analyze/chords", json={"file_path": a})
    assert r1.status_code == 200, r1.text
    assert [name for name, _ in seen] == ["lookup", "store"]

    ra._chords_cache.clear()                       # "restart": read it back from disk
    r2 = client.post("/analyze/chords", json={"file_path": a})
    assert r2.status_code == 200 and r2.json() == r1.json()
    assert [name for name, _ in seen] == ["lookup", "store", "lookup"]

    on_loop = [name for name, loop in seen if loop]
    assert not on_loop, f"disk-cache I/O ran on the event loop: {on_loop}"


def test_no_async_handler_touches_the_disk_cache_directly():
    """Every ``async def`` in the service: a disk-cache call may be handed to
    an executor (``run_blocking(_acache.lookup, ...)``) or sit inside a nested
    worker ``def``, never be called in the coroutine's own body."""
    problems = []
    for py in sorted(_PKG.rglob("*.py")):
        tree = ast.parse(py.read_text(encoding="utf-8"), filename=str(py))
        for fn in ast.walk(tree):
            if not isinstance(fn, ast.AsyncFunctionDef):
                continue
            stack = list(fn.body)
            while stack:
                node = stack.pop()
                if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef, ast.Lambda)):
                    continue                       # runs wherever it is called from
                if isinstance(node, ast.Call):
                    f = node.func
                    if isinstance(f, ast.Attribute) and isinstance(f.value, ast.Name) \
                            and f.value.id in _CACHE_ALIASES and f.attr in _DISK_IO:
                        problems.append(f"{py.relative_to(_PKG)}:{node.lineno} "
                                        f"{fn.name}() calls {f.value.id}.{f.attr}() on the loop")
                stack.extend(ast.iter_child_nodes(node))
    assert not problems, "\n".join(problems)


# --------------------------------------------------------------------------- #
# lyrics
# --------------------------------------------------------------------------- #

def _fake_asr(monkeypatch):
    from khensu_services.analysis import lyric_analyzer as la
    from khensu_services.analysis import router_audio as ra

    ra._lyrics_cache.clear()
    calls: list[str] = []

    def transcribe(file_path, model_size="base"):
        calls.append(model_size)
        return [{"word": f"hello-{len(calls)}", "start_sec": 0.5, "end_sec": 0.9,
                 "probability": 0.9}]

    monkeypatch.setattr(la, "transcribe_lyrics", transcribe)
    return calls


def test_lyrics_are_not_written_to_the_disk_tier(client, tmp_path, monkeypatch):
    calls = _fake_asr(monkeypatch)
    a = str(_wav(tmp_path / "a.wav"))
    before = ac.stats()

    r = client.post("/analyze/lyrics", json={"file_path": a})
    assert r.status_code == 200, r.text
    assert calls == ["base"]

    after = ac.stats()
    assert after["puts"] == before["puts"], "the lyric scan wrote a disk entry"
    assert "analyze_lyrics" not in {row["analyzer"] for row in after["analyzers"]}


def test_lyrics_repeat_request_is_served_from_memory(client, tmp_path, monkeypatch):
    calls = _fake_asr(monkeypatch)
    a = str(_wav(tmp_path / "a.wav"))

    r1 = client.post("/analyze/lyrics", json={"file_path": a})
    r2 = client.post("/analyze/lyrics", json={"file_path": a})
    assert r1.status_code == 200 and r2.status_code == 200
    assert r2.json() == r1.json()
    assert calls == ["base"], "the second request transcribed again"

    # The model size changes the transcript: its own entry.
    r3 = client.post("/analyze/lyrics", json={"file_path": a, "model_size": "tiny"})
    assert r3.status_code == 200
    assert calls == ["base", "tiny"]


def test_lyrics_are_transcribed_again_after_a_restart(client, tmp_path, monkeypatch):
    """A restart may come with different Whisper weights; nothing on disk may
    answer for them."""
    from khensu_services.analysis import router_audio as ra

    calls = _fake_asr(monkeypatch)
    a = str(_wav(tmp_path / "a.wav"))
    r1 = client.post("/analyze/lyrics", json={"file_path": a})
    assert r1.status_code == 200 and calls == ["base"]

    ra._lyrics_cache.clear()                       # the memory tier dies with the process
    r2 = client.post("/analyze/lyrics", json={"file_path": a})
    assert r2.status_code == 200
    assert calls == ["base", "base"]
    assert r2.json()["words"][0]["word"] == "hello-2"


def test_lyrics_without_the_asr_extra_is_503_and_not_cached(client, tmp_path, monkeypatch):
    from khensu_services.analysis import lyric_analyzer as la
    from khensu_services.analysis import router_audio as ra

    ra._lyrics_cache.clear()
    calls: list[int] = []

    def missing(file_path, model_size="base"):
        calls.append(1)
        raise la.ASRNotAvailableError("faster-whisper is not installed")

    monkeypatch.setattr(la, "transcribe_lyrics", missing)
    a = str(_wav(tmp_path / "a.wav"))
    assert client.post("/analyze/lyrics", json={"file_path": a}).status_code == 503
    assert client.post("/analyze/lyrics", json={"file_path": a}).status_code == 503
    assert calls == [1, 1]
    assert len(ra._lyrics_cache) == 0
