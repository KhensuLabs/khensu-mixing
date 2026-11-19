"""CE-PLT-2: the content-addressed persistent analysis cache and changed-
region detection (the conftest points every test at its own cache dir)."""
from __future__ import annotations

import os
import threading
from pathlib import Path

import numpy as np
import pytest
import soundfile as sf

from khensu_services.common import analysis_cache as ac


def _wav(path: Path, y: np.ndarray, sr: int = 22050) -> Path:
    sf.write(str(path), y.T if y.ndim == 2 else y, sr, subtype="FLOAT")
    return path


def _tone(seconds: float, sr: int = 22050, freq: float = 440.0, amp: float = 0.3) -> np.ndarray:
    t = np.arange(int(seconds * sr)) / sr
    mono = amp * np.sin(2 * np.pi * freq * t)
    rng = np.random.default_rng(1)
    return np.stack([mono, 0.8 * mono + 0.01 * rng.standard_normal(mono.size)]).astype(np.float32)


# --------------------------------------------------------------------------- #
# Keys
# --------------------------------------------------------------------------- #

def test_key_is_content_addressed_not_path_addressed(tmp_path):
    y = _tone(0.5)
    a = _wav(tmp_path / "a.wav", y)
    b = _wav(tmp_path / "copy.wav", y)
    assert ac.key_for(a, "x", "1", {"p": 1}) == ac.key_for(b, "x", "1", {"p": 1})


def test_key_changes_when_content_changes(tmp_path):
    a = _wav(tmp_path / "a.wav", _tone(0.5))
    k1 = ac.key_for(a, "x", "1", None)
    _wav(a, _tone(0.5, freq=550.0))          # re-rendered in place
    assert ac.key_for(a, "x", "1", None) != k1


@pytest.mark.parametrize("change", [
    ({"p": 2}, "x", "1"),       # a parameter
    ({"p": 1}, "y", "1"),       # the analyzer
    ({"p": 1}, "x", "2"),       # the analyzer version
])
def test_key_changes_with_params_analyzer_and_version(tmp_path, change):
    a = _wav(tmp_path / "a.wav", _tone(0.3))
    params, name, ver = change
    assert ac.key_for(a, "x", "1", {"p": 1}) != ac.key_for(a, name, ver, params)


def test_param_order_and_tuples_are_canonical(tmp_path):
    a = _wav(tmp_path / "a.wav", _tone(0.3))
    assert ac.key_for(a, "x", "1", {"a": 1, "b": (1, 2)}) == \
        ac.key_for(a, "x", "1", {"b": [1, 2], "a": 1})


def test_service_version_is_part_of_the_key(tmp_path, monkeypatch):
    a = _wav(tmp_path / "a.wav", _tone(0.3))
    k1 = ac.key_for(a, "x", "1", None)
    import khensu_services.version as v
    monkeypatch.setattr(v, "SERVICE_VERSION", v.SERVICE_VERSION + ".test")
    assert ac.key_for(a, "x", "1", None) != k1


def test_missing_file_or_disabled_cache_gives_no_key(tmp_path, monkeypatch):
    assert ac.key_for(tmp_path / "nope.wav", "x", "1", None) is None
    a = _wav(tmp_path / "a.wav", _tone(0.3))
    monkeypatch.setenv("KHENSU_ANALYSIS_CACHE", "0")
    assert ac.key_for(a, "x", "1", None) is None


# --------------------------------------------------------------------------- #
# cached_call / invalidation
# --------------------------------------------------------------------------- #

def test_cached_call_hits_and_invalidates_on_parameter_change(tmp_path):
    a = _wav(tmp_path / "a.wav", _tone(0.3))
    calls: list[int] = []

    def compute(v):
        def _f():
            calls.append(v)
            return {"value": v, "arr": np.float64(1.5), "flag": np.bool_(True)}
        return _f

    r1 = ac.cached_call(a, "demo", "1", {"block": 1}, compute(1))
    r2 = ac.cached_call(a, "demo", "1", {"block": 1}, compute(99))
    assert r1["value"] == 1 and r2 == {"value": 1, "arr": 1.5, "flag": True}
    assert calls == [1]
    r3 = ac.cached_call(a, "demo", "1", {"block": 2}, compute(2))   # parameter changed
    assert r3["value"] == 2 and calls == [1, 2]


def test_cache_survives_a_new_process_instance(tmp_path):
    a = _wav(tmp_path / "a.wav", _tone(0.3))
    ac.cached_call(a, "demo", "1", None, lambda: {"v": 7})
    fresh = ac.AnalysisCache(ac.default_root(), ac.budget_bytes())   # "after a restart"
    key = ac.key_for(a, "demo", "1", None)
    assert fresh.get("demo", key) == {"v": 7}


def test_unserialisable_values_are_simply_not_cached(tmp_path):
    a = _wav(tmp_path / "a.wav", _tone(0.3))
    calls = []
    ac.cached_call(a, "demo", "1", None, lambda: calls.append(1) or {"o": object()})
    ac.cached_call(a, "demo", "1", None, lambda: calls.append(1) or {"o": object()})
    assert len(calls) == 2


def test_corrupt_entry_is_a_miss_and_is_removed(tmp_path):
    cache = ac.AnalysisCache(tmp_path / "c", 10_000_000)
    key = "ab" + "0" * 62
    assert cache.put("demo", key, {"v": 1})
    path = cache._path("demo", key)
    path.write_text("{not json", encoding="utf-8")
    assert cache.get("demo", key) is None
    assert not path.exists()


# --------------------------------------------------------------------------- #
# Budget / LRU / concurrency
# --------------------------------------------------------------------------- #

def test_lru_eviction_keeps_the_store_under_budget(tmp_path):
    cache = ac.AnalysisCache(tmp_path / "c", 6_000)
    payload = {"x": "y" * 900}
    keys = [f"{i:02x}" + "1" * 62 for i in range(12)]
    for i, k in enumerate(keys):
        assert cache.put("demo", k, payload)
        os.utime(cache._path("demo", k), (1_000_000 + i, 1_000_000 + i))
        if i == 3:
            # Touch the first entry: it becomes most recently used.
            assert cache.get("demo", keys[0]) == payload
    st = cache.stats()
    assert st["bytes"] <= 6_000
    assert st["evictions"] > 0
    assert cache.get("demo", keys[-1]) == payload          # newest kept
    assert cache.get("demo", keys[1]) is None              # old, untouched: evicted


def test_concurrent_writers_never_produce_a_torn_entry(tmp_path):
    cache = ac.AnalysisCache(tmp_path / "c", 50_000_000)
    key = "cd" + "2" * 62
    big = {"v": list(range(5000))}
    errors: list[BaseException] = []

    def writer():
        try:
            for _ in range(20):
                cache.put("demo", key, big)
        except BaseException as exc:  # pragma: no cover - surfaced below
            errors.append(exc)

    def reader():
        try:
            for _ in range(60):
                got = cache.get("demo", key)
                assert got is None or got == big
        except BaseException as exc:
            errors.append(exc)

    threads = [threading.Thread(target=writer) for _ in range(4)] + \
              [threading.Thread(target=reader) for _ in range(4)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    assert not errors
    assert cache.get("demo", key) == big
    leftovers = [p for p in (tmp_path / "c").rglob("*.tmp")]
    assert leftovers == []


def test_clear_and_stats(tmp_path):
    a = _wav(tmp_path / "a.wav", _tone(0.3))
    ac.cached_call(a, "one", "1", None, lambda: {"v": 1})
    ac.cached_call(a, "two", "1", None, lambda: {"v": 2})
    st = ac.stats()
    assert st["entries"] == 2 and {r["analyzer"] for r in st["analyzers"]} == {"one", "two"}
    assert ac.clear() == 2
    assert ac.stats()["entries"] == 0


# --------------------------------------------------------------------------- #
# Routes
# --------------------------------------------------------------------------- #

def test_cache_routes(client, tmp_path):
    a = _wav(tmp_path / "a.wav", _tone(0.3))
    ac.cached_call(a, "demo", "1", None, lambda: {"v": 1})
    r = client.get("/analysis/cache")
    assert r.status_code == 200
    body = r.json()
    assert body["enabled"] is True and body["entries"] == 1
    assert body["budget_bytes"] == ac.budget_bytes()
    r = client.post("/analysis/cache/clear")
    assert r.status_code == 200 and r.json() == {"removed": 1}
    assert client.get("/analysis/cache").json()["entries"] == 0


def test_analyze_quick_result_persists_across_memory_cache_loss(client, tmp_path, monkeypatch):
    from khensu_services.analysis import quick_analyzer as qa
    from khensu_services.analysis import router_audio as ra
    a = _wav(tmp_path / "a.wav", _tone(1.0))
    calls = []
    fake = {"mode": "quick", "bpm": 120.0, "bpm_confidence": 0.5, "beat_times": [],
            "downbeat_times": None, "key": "A minor", "key_confidence": 0.5,
            "sections": [], "pitch_notes": None, "analysis_duration_ms": 1.0}
    monkeypatch.setattr(qa, "analyze_quick", lambda *a_, **k: calls.append(1) or dict(fake))
    r1 = client.post("/analyze", json={"file_path": str(a), "mode": "quick"})
    assert r1.status_code == 200 and calls == [1]
    ra._analysis_cache.clear()           # "service restart": memory tier gone
    r2 = client.post("/analyze", json={"file_path": str(a), "mode": "quick"})
    assert r2.status_code == 200 and calls == [1]
    assert r2.json()["bpm"] == 120.0
    # Full intake's / sections' shared quick pass reads the same entry.
    assert ra.cached_analyze_quick(str(a))["key"] == "A minor" and calls == [1]


def test_drums_scan_is_cached(client, tmp_path, monkeypatch):
    from khensu_services.analysis import drum_analyzer
    a = _wav(tmp_path / "a.wav", _tone(1.0))
    real = drum_analyzer.analyze_drums
    calls = []
    monkeypatch.setattr(drum_analyzer, "analyze_drums",
                        lambda y, sr: calls.append(1) or real(y, sr))
    r1 = client.post("/analyze/drums", json={"file_path": str(a)})
    r2 = client.post("/analyze/drums", json={"file_path": str(a)})
    assert r1.status_code == r2.status_code == 200
    assert r1.json() == r2.json()
    assert calls == [1]


# --------------------------------------------------------------------------- #
# Changed regions
# --------------------------------------------------------------------------- #

def test_identical_files_have_no_changed_regions(tmp_path):
    from khensu_services.analysis.changed_regions import find_changed_regions
    y = _tone(2.0)
    a = _wav(tmp_path / "a.wav", y)
    b = _wav(tmp_path / "b.wav", y)
    r = find_changed_regions(str(a), str(b))
    assert r["comparable"] and r["regions"] == [] and r["method"] == "identical_bytes"


def test_changed_region_is_located(tmp_path):
    from khensu_services.analysis.changed_regions import find_changed_regions
    sr = 22050
    y = _tone(6.0, sr)
    z = y.copy()
    z[:, int(2.0 * sr):int(3.0 * sr)] *= 0.5        # a 1 s edit at 2-3 s
    a = _wav(tmp_path / "a.wav", y, sr)
    b = _wav(tmp_path / "b.wav", z, sr)
    r = find_changed_regions(str(a), str(b), pad_ms=0.0, merge_gap_ms=0.0)
    assert r["comparable"] and len(r["regions"]) == 1
    reg = r["regions"][0]
    assert 1.9 <= reg["start_sec"] <= 2.0 and 3.0 <= reg["end_sec"] <= 3.1
    assert 0.15 < r["changed_fraction"] < 0.2


def test_sub_threshold_difference_is_not_a_change(tmp_path):
    from khensu_services.analysis.changed_regions import find_changed_regions
    y = _tone(1.0)
    z = y + np.float32(1e-6)                      # ~-120 dBFS: re-encode noise
    r = find_changed_regions(str(_wav(tmp_path / "a.wav", y)), str(_wav(tmp_path / "b.wav", z)))
    assert r["comparable"] and r["regions"] == [] and r["method"] == "block_difference"


def test_incomparable_files_report_everything_changed(tmp_path):
    from khensu_services.analysis.changed_regions import find_changed_regions
    a = _wav(tmp_path / "a.wav", _tone(1.0), 22050)
    b = _wav(tmp_path / "b.wav", _tone(1.0, 44100), 44100)
    r = find_changed_regions(str(a), str(b))
    assert r["comparable"] is False and "sample rates" in r["reason"]
    assert r["changed_fraction"] == 1.0 and len(r["regions"]) == 1


def test_changed_regions_route(client, tmp_path):
    sr = 22050
    y = _tone(3.0, sr)
    z = y.copy()
    z[:, sr:2 * sr] = 0.0
    a = _wav(tmp_path / "a.wav", y, sr)
    b = _wav(tmp_path / "b.wav", z, sr)
    r = client.post("/analysis/changed_regions", json={"file_a": str(a), "file_b": str(b)})
    assert r.status_code == 200, r.text
    body = r.json()
    assert body["comparable"] is True and len(body["regions"]) == 1
    r = client.post("/analysis/changed_regions",
                    json={"file_a": str(a), "file_b": str(tmp_path / "missing.wav")})
    assert r.status_code == 404


def test_incremental_metric_timeline_matches_a_full_pass(client, tmp_path):
    """Post-render re-measure: only windows touching the edit are recomputed,
    and the result matches a whole-file measurement."""
    sr = 22050
    y = _tone(20.0, sr)
    z = y.copy()
    z[:, int(12 * sr):int(13 * sr)] *= 0.25
    src = _wav(tmp_path / "src.wav", y, sr)
    render = _wav(tmp_path / "render.wav", z, sr)

    inc = client.post("/analyze/metric_timeline",
                      json={"file_path": str(render), "baseline_path": str(src)})
    assert inc.status_code == 200, inc.text
    inc = inc.json()
    full = client.post("/analyze/metric_timeline", json={"file_path": str(render)}).json()

    assert inc["n_windows"] == full["n_windows"]
    assert 0 < inc["recomputed_windows"] < inc["n_windows"]
    assert inc["reused_windows"] == inc["n_windows"] - inc["recomputed_windows"]
    for wi, wf in zip(inc["windows"], full["windows"]):
        assert wi["start_sec"] == wf["start_sec"]
        for k in ("loudness_lufs", "crest_db"):
            assert wi[k] == pytest.approx(wf[k], abs=0.05), (k, wi, wf)
        assert wi["harshness"] == pytest.approx(wf["harshness"], abs=0.01)
    assert inc["hotspots"]["most_squashed"]["window_index"] == \
        full["hotspots"]["most_squashed"]["window_index"]


def test_incremental_metric_timeline_falls_back_when_incomparable(client, tmp_path):
    src = _wav(tmp_path / "src.wav", _tone(8.0, 22050), 22050)
    render = _wav(tmp_path / "render.wav", _tone(9.0, 22050), 22050)   # length changed
    r = client.post("/analyze/metric_timeline",
                    json={"file_path": str(render), "baseline_path": str(src)})
    assert r.status_code == 200, r.text
    body = r.json()
    assert body["reused_windows"] == 0 and body["recomputed_windows"] == body["n_windows"]
