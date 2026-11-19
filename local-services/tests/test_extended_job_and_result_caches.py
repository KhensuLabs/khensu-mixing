"""/analyze/start (advanced) returns the extended result; /analyze/extended
reuses caches; translation crest term (M3); new result caches (E21);
process-wide decode cache (E20); AnalyzeResponse energy/mood (M2)."""
from __future__ import annotations

import time

import numpy as np
import pytest
import soundfile as sf

import khensu_services.analysis.router as main_router
import khensu_services.analysis.router_audio as ra
from khensu_services.analysis.models import AnalyzeResponse, ExtendedAnalysisResponse

SR = 22050


def _base(mode="advanced"):
    return {"mode": mode, "bpm": 120.0, "bpm_confidence": 0.9, "beat_times": [0.5, 1.0],
            "downbeat_times": [0.5], "key": "C major", "key_confidence": 0.8,
            "sections": [{"start_sec": 0.0, "label": "A"}], "pitch_notes": None,
            "analysis_duration_ms": 1.0, "energy": 61.5, "mood": "euphoric"}


@pytest.fixture
def stereo_file(tmp_path):
    rng = np.random.default_rng(3)
    p = tmp_path / "mix.wav"
    sf.write(str(p), (0.2 * rng.standard_normal((SR * 3, 2))).astype(np.float32), SR)
    return p


@pytest.fixture
def fast_base(monkeypatch):
    calls = {"advanced": 0}

    def adv(path, progress_cb=None):
        calls["advanced"] += 1
        if progress_cb:
            progress_cb("loading_audio", 1, 9)
        return _base()

    monkeypatch.setattr(main_router, "analyze_advanced", adv)
    monkeypatch.setattr(main_router, "analyze_quick", lambda p: _base("quick"))
    ra._analysis_cache.clear()
    ra._extended_cache.clear()
    return calls


def _wait(client, job_id, timeout=120.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        body = client.get(f"/analyze/status/{job_id}").json()
        if body["status"] in ("completed", "failed", "cancelled"):
            return body
        time.sleep(0.1)
    raise AssertionError("job did not finish")


def test_advanced_job_result_is_the_extended_result(client, stereo_file, fast_base):
    r = client.post("/analyze/start", json={"file_path": str(stereo_file), "mode": "advanced"})
    assert r.status_code == 200
    job = _wait(client, r.json()["job_id"])
    assert job["status"] == "completed", job
    result = job["result"]
    for key in ("harshness_score", "translation_risk", "genre_blend", "mono_compat"):
        assert key in result
    assert job["total_stages"] == 10 and job["stage_index"] == 10
    ExtendedAnalysisResponse.model_validate(result)
    assert result["energy"] == 61.5 and result["mood"] == "euphoric"

    # /analyze/extended now serves the cached extended result: no new base pass.
    before = fast_base["advanced"]
    r2 = client.post("/analyze/extended", json={"file_path": str(stereo_file), "mode": "advanced"})
    assert r2.status_code == 200
    assert fast_base["advanced"] == before
    assert r2.json()["harshness_score"] == result["harshness_score"]

    # A second start is a cache hit straight to the extended result.
    r3 = client.post("/analyze/start", json={"file_path": str(stereo_file), "mode": "advanced"})
    job3 = client.get(f"/analyze/status/{r3.json()['job_id']}").json()
    assert job3["status"] == "completed" and "harshness_score" in job3["result"]


def test_extended_reuses_cached_base_analysis(client, stereo_file, fast_base):
    r = client.post("/analyze", json={"file_path": str(stereo_file), "mode": "advanced"})
    assert r.status_code == 200
    assert r.json()["energy"] == 61.5 and r.json()["mood"] == "euphoric"   # M2
    assert fast_base["advanced"] == 1
    r2 = client.post("/analyze/extended", json={"file_path": str(stereo_file), "mode": "advanced"})
    assert r2.status_code == 200, r2.text
    assert fast_base["advanced"] == 1


def test_quick_job_result_stays_base(client, stereo_file, fast_base):
    r = client.post("/analyze/start", json={"file_path": str(stereo_file), "mode": "quick"})
    job = _wait(client, r.json()["job_id"])
    assert job["status"] == "completed"
    assert "harshness_score" not in job["result"]


def test_translation_risk_receives_measured_crest(stereo_file, fast_base, monkeypatch):
    seen = {}
    real = ra.analyze_translation_risk

    def spy(mono, spectral, dynamics):
        seen.update(dynamics)
        return real(mono, spectral, dynamics)

    monkeypatch.setattr(ra, "analyze_translation_risk", spy)
    ra.compute_extended_result(str(stereo_file), "advanced", base_result=_base())
    # Gaussian noise has a crest of ~12-14 dB; the old code always passed 10.0.
    assert seen["crest_factor_db"] != 10.0
    assert 8.0 < seen["crest_factor_db"] < 20.0


def test_analyze_response_declares_energy_mood_felt_bpm():
    fields = AnalyzeResponse.model_fields
    for name in ("energy", "mood", "felt_bpm"):
        assert name in fields and name in ExtendedAnalysisResponse.model_fields


def test_chords_and_mix_quality_results_are_cached(client, stereo_file, monkeypatch):
    import khensu_services.analysis.performance_analyzer as pa

    calls = {"n": 0}
    real = pa.detect_chord_sequence

    def counting(*a, **k):
        calls["n"] += 1
        return real(*a, **k)

    monkeypatch.setattr(pa, "detect_chord_sequence", counting)
    assert client.post("/analyze/chords", json={"file_path": str(stereo_file)}).status_code == 200
    assert client.post("/analyze/chords", json={"file_path": str(stereo_file)}).status_code == 200
    assert calls["n"] == 1

    from khensu_services.common.file_analysis import ANALYSES
    n = {"c": 0}
    monkeypatch.setattr(ANALYSES["mix_quality"], "compute", lambda req: n.__setitem__("c", n["c"] + 1) or {"available": False, "reason": "fake"})
    for _ in range(2):
        assert client.post("/analyze/mix_quality", json={"file_path": str(stereo_file)}).json() == {
            "available": False, "reason": "fake"}
    assert n["c"] == 1


def test_performance_results_cached_and_invalidated_on_change(client, stereo_file, monkeypatch):
    import khensu_services.analysis.performance_analyzer as pa

    n = {"c": 0}

    def fake(path):
        n["c"] += 1
        return {"rt60_sec": None, "decay_segments": n["c"]}

    monkeypatch.setattr(pa, "estimate_reverb_tail", fake)
    a = client.post("/analyze/reverb_tail", json={"file_path": str(stereo_file)}).json()
    b = client.post("/analyze/reverb_tail", json={"file_path": str(stereo_file)}).json()
    assert a == b and n["c"] == 1
    rng = np.random.default_rng(9)
    sf.write(str(stereo_file), (0.1 * rng.standard_normal((SR * 4, 2))).astype(np.float32), SR)
    client.post("/analyze/reverb_tail", json={"file_path": str(stereo_file)})
    assert n["c"] == 2


def test_process_wide_decode_cache_is_shared_and_readonly(stereo_file):
    from khensu_services.analysis.context import AnalysisContext, decoded_audio_cache

    decoded_audio_cache.clear()
    y1, sr1 = AnalysisContext(str(stereo_file)).load(sr=None, mono=False)
    ctx2 = AnalysisContext(str(stereo_file))
    y2, sr2 = ctx2.load(sr=None, mono=False)
    assert ctx2.decode_count == 0 and y2 is y1 and sr1 == sr2
    with pytest.raises(ValueError):
        y2[0, 0] = 1.0
    import librosa
    ref, _ = librosa.load(str(stereo_file), sr=None, mono=False)
    np.testing.assert_array_equal(ref, y1)


def test_decode_cache_respects_byte_budget():
    from khensu_services.analysis.context import _DecodedAudioCache

    cache = _DecodedAudioCache(max_bytes=1000)
    arr = np.zeros(100, dtype=np.float32)   # 400 bytes
    for i in range(4):
        cache.put(("k", i), (arr.copy(), 1))
    assert cache.total_bytes <= 1000 and len(cache) == 2
    assert cache.get(("k", 0)) is None and cache.get(("k", 3)) is not None
    cache.put(("big",), (np.zeros(1000, np.float32), 1))   # oversized: not cached
    assert cache.get(("big",)) is None


def test_fix_check_job_skips_the_extras_no_finding_reads(client, stereo_file, fast_base, monkeypatch):
    """A fix check (FEAT-1) leaves out the emotional arc, translation
    prediction, stereo-field timeline and benchmark, keeps every input a
    finding is built from, and is cached apart from the full result."""
    def boom(*_a, **_k):
        raise AssertionError("a fix check must not run this")

    monkeypatch.setattr(ra, "analyze_emotional_arc", boom)
    monkeypatch.setattr(ra, "analyze_translation", boom)
    monkeypatch.setattr(ra, "analyze_stereo_field_over_time", boom)
    monkeypatch.setattr(ra, "benchmark_against_references", boom)

    body = {"file_path": str(stereo_file), "mode": "advanced", "purpose": "fix_check"}
    job = _wait(client, client.post("/analyze/start", json=body).json()["job_id"])
    assert job["status"] == "completed", job
    result = job["result"]
    ExtendedAnalysisResponse.model_validate(result)
    for key in ("harshness_score", "translation_risk", "genre_blend", "mono_compat",
                "mid_side_imbalance", "resonance_hotspots"):
        assert key in result
    for key in ("emotional_arc", "translation_prediction", "stereo_field_timeline", "benchmark"):
        assert result.get(key) is None

    # The next fix check of the same file is a cache hit.
    job2 = client.get(f"/analyze/status/{client.post('/analyze/start', json=body).json()['job_id']}").json()
    assert job2["status"] == "completed" and job2["stage"] == "cached"

    # It never stands in for a full result: a full job still runs the extras.
    calls = []
    monkeypatch.setattr(ra, "analyze_emotional_arc",
                        lambda *a, **k: calls.append(1) or {"points": [], "flat_sections": [],
                                                            "max_energy_delta": 0.0})
    monkeypatch.setattr(ra, "analyze_translation", lambda *a, **k: {"per_profile": {}})
    monkeypatch.setattr(ra, "analyze_stereo_field_over_time", lambda *a, **k: {"points": []})
    monkeypatch.setattr(ra, "benchmark_against_references", lambda *a, **k: None)
    full = _wait(client, client.post("/analyze/start", json={**body, "purpose": "full"}).json()["job_id"])
    assert full["status"] == "completed" and calls == [1]
    assert full["result"]["emotional_arc"] is not None


def test_fix_check_job_reuses_a_cached_full_result(client, stereo_file, fast_base):
    full_body = {"file_path": str(stereo_file), "mode": "advanced"}
    assert _wait(client, client.post("/analyze/start", json=full_body).json()["job_id"])["status"] == "completed"
    r = client.post("/analyze/start", json={**full_body, "purpose": "fix_check"})
    job = client.get(f"/analyze/status/{r.json()['job_id']}").json()
    assert job["status"] == "completed" and job["stage"] == "cached"
