"""PERF-2 / PERF-3: the Dashboard's follow-up scans, the chord scan and the
trust map survive a service restart (the disk tier), intake fills the same
entries the Dashboard reads, and the remaining direct decodes go through the
shared decode cache. The conftest points every test at its own cache dir."""
from __future__ import annotations

from pathlib import Path

import numpy as np
import pytest
import soundfile as sf


def _wav(path: Path, seconds: float = 1.5, sr: int = 22050) -> Path:
    t = np.arange(int(seconds * sr)) / sr
    mono = 0.3 * np.sin(2 * np.pi * 220.0 * t)
    sf.write(str(path), np.stack([mono, 0.9 * mono]).T.astype(np.float32), sr, subtype="FLOAT")
    return path


# (route, request body builder, file-analysis table name - ENG-5)
_SCANS = [
    ("/analyze/signal_integrity", lambda p: {"file_path": p},
     "signal_integrity"),
    ("/analyze/pan_balance", lambda p: {"file_path": p},
     "pan_balance"),
    ("/analyze/source_triage", lambda p: {"file_path": p},
     "source_triage"),
    ("/analyze/streaming_normalization", lambda p: {"file_path": p},
     "streaming_normalization"),
    ("/analyze/tuning_reference", lambda p: {"file_path": p},
     "tuning_reference"),
    ("/analyze/mono_compat", lambda p: {"file_path": p},
     "mono_compat"),
    ("/analyze/quick_stats", lambda p: {"file_path": p},
     "quick_stats"),
    ("/analyze/measurement_quality", lambda p: {"file_path": p},
     "measurement_quality"),
]


@pytest.mark.parametrize("route,body,name", _SCANS, ids=[s[0] for s in _SCANS])
def test_scan_survives_a_service_restart(client, tmp_path, route, body, name):
    from khensu_services.common import analysis_cache as ac
    from khensu_services.common.file_analysis import ANALYSES
    a = str(_wav(tmp_path / "a.wav"))
    r1 = client.post(route, json=body(a))
    assert r1.status_code == 200, r1.text
    before = ac.stats()["hits"]
    ANALYSES[name].cache.clear()         # "service restart": memory tier gone
    r2 = client.post(route, json=body(a))
    assert r2.status_code == 200
    assert r2.json() == r1.json()
    assert ac.stats()["hits"] > before, "second call should read the disk tier"


def test_chords_survive_a_service_restart(client, tmp_path, monkeypatch):
    from khensu_services.analysis import _key
    from khensu_services.analysis import router_audio as ra

    a = str(_wav(tmp_path / "a.wav", seconds=3.0))
    real = _key.safe_chroma_cqt
    calls = []
    monkeypatch.setattr(_key, "safe_chroma_cqt", lambda *a_, **k: calls.append(1) or real(*a_, **k))
    r1 = client.post("/analyze/chords", json={"file_path": a})
    assert r1.status_code == 200 and calls == [1]
    ra._chords_cache.clear()
    r2 = client.post("/analyze/chords", json={"file_path": a})
    assert r2.status_code == 200 and calls == [1]
    assert r2.json() == r1.json()


def test_intake_fills_the_entries_the_dashboard_reads(tmp_path, monkeypatch):
    """Quick stats, signal integrity and pan balance measured at intake are
    read back by their own endpoints' keys, not measured again."""
    from khensu_services.analysis import quick_stats, signal_integrity, pan_balance
    from khensu_services.common import analysis_cache as ac

    a = str(_wav(tmp_path / "a.wav"))
    stored = {
        "quick_stats": ac.persistent_call(a, "quick_stats", None,
                                          quick_stats.analyze_quick_stats, a, None),
        "signal_integrity": ac.persistent_call(a, "signal_integrity", None,
                                               signal_integrity.analyze_signal_integrity, a, None),
        "pan_balance": ac.persistent_call(a, "pan_balance", {"window_sec": 1.0},
                                          pan_balance.analyze_pan_balance, a, 1.0, None),
    }

    def boom(*_a, **_k):
        raise AssertionError("measured twice")

    monkeypatch.setattr(quick_stats, "analyze_quick_stats", boom)
    monkeypatch.setattr(signal_integrity, "analyze_signal_integrity", boom)
    monkeypatch.setattr(pan_balance, "analyze_pan_balance", boom)
    # The endpoints' keys: same analyzer name, same params.
    assert ac.persistent_call(a, "quick_stats", None, boom) == stored["quick_stats"]
    assert ac.persistent_call(a, "signal_integrity", None, boom) == stored["signal_integrity"]
    assert ac.persistent_call(a, "pan_balance", {"window_sec": round(1.0, 3)}, boom) \
        == stored["pan_balance"]


def test_full_intake_stages_use_the_disk_tier():
    """Guard the wiring: intake's three shared stages name the endpoints'
    cache entries (a rename on one side would silently measure twice)."""
    import inspect

    from khensu_services.analysis import router_full_intake

    src = inspect.getsource(router_full_intake)
    for name in ('"quick_stats"', '"signal_integrity"', '"pan_balance"'):
        assert f"persistent_call(\n            file_path, {name}" in src, name


def test_direct_decodes_go_through_the_shared_cache(tmp_path, monkeypatch):
    """Sections, drums, the reference metrics, tuning and the deep job decode
    through AnalysisContext: a second scan of the same file decodes nothing."""
    from khensu_services.analysis import context
    from khensu_services.analysis.tuning_reference import analyze_tuning_reference
    from khensu_services.analysis.router_audio import _extract_mix_metrics

    a = str(_wav(tmp_path / "a.wav", seconds=2.0))
    _extract_mix_metrics(a)              # stereo decode
    analyze_tuning_reference(a)          # mono decode
    import librosa

    def no_decode(*_a, **_k):
        raise AssertionError("decoded again")

    monkeypatch.setattr(librosa, "load", no_decode)
    _extract_mix_metrics(a)
    analyze_tuning_reference(a)
    y, _sr = context.load_audio(a, sr=None, mono=False)
    assert not y.flags.writeable
