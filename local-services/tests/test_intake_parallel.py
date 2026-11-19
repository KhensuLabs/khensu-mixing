"""Concurrent intake stages (pipeline enhancement #2).

The five mix-level stages are mutually independent and now run concurrently.
These tests pin the two properties that must survive the change: the
independent stages actually overlap in time (not serialized), and a failing
stage is still isolated so the bundle completes with that one marked failed.
The dependency tail (musical_context -> stem_context) and cancellation are
covered by test_full_intake.py.
"""
from __future__ import annotations

import threading
import time

import numpy as np
import soundfile as sf

import khensu_services.analysis.router_full_intake as fi


def _tiny_wav(path, sr=22050, secs=0.5):
    t = np.arange(int(sr * secs)) / sr
    y = np.stack([0.2 * np.sin(2 * np.pi * 220 * t)] * 2, axis=1).astype(np.float32)
    sf.write(str(path), y, sr)
    return str(path)


def _patch_all_stages(monkeypatch, quick_stats_fn, signal_integrity_fn):
    """Replace every stage analyzer _run_intake imports with a light stub, so
    the orchestration is what's under test, not the DSP."""
    monkeypatch.setattr("khensu_services.analysis.quick_stats.analyze_quick_stats",
                        quick_stats_fn)
    monkeypatch.setattr("khensu_services.analysis.signal_integrity.analyze_signal_integrity",
                        signal_integrity_fn)
    monkeypatch.setattr("khensu_services.analysis.source_triage.triage_source",
                        lambda *a, **k: {"stage": "source_triage"})
    monkeypatch.setattr("khensu_services.analysis.pan_balance.analyze_pan_balance",
                        lambda *a, **k: {"stage": "pan_balance"})
    monkeypatch.setattr("khensu_services.analysis.artifacts.analyze_artifacts",
                        lambda *a, **k: {"stage": "artifacts"})
    # Tail stage deps (musical_context); stem_context self-defers without stems.
    monkeypatch.setattr("khensu_services.analysis.quick_analyzer.analyze_quick",
                        lambda *a, **k: {"stage": "quick"})
    monkeypatch.setattr("khensu_services.analysis.genre_context.build_genre_context",
                        lambda *a, **k: {"selected_genre_id": "pop"})


def test_independent_stages_run_concurrently(tmp_path, monkeypatch):
    src = _tiny_wav(tmp_path / "a.wav")
    events: list[tuple[str, str, float]] = []
    lock = threading.Lock()

    def slow(name):
        def fn(*a, **k):
            with lock:
                events.append((name, "enter", time.perf_counter()))
            time.sleep(0.3)
            with lock:
                events.append((name, "exit", time.perf_counter()))
            return {"stage": name}
        return fn

    _patch_all_stages(monkeypatch, slow("quick_stats"), slow("signal_integrity"))

    fi._run_intake(src, "", "", True, False, "none", {}, {})

    span = {n: {} for n in ("quick_stats", "signal_integrity")}
    for name, phase, ts in events:
        if name in span:
            span[name][phase] = ts
    qs, si = span["quick_stats"], span["signal_integrity"]
    # Intervals overlap -> ran concurrently, not one-after-the-other.
    assert qs["enter"] < si["exit"] and si["enter"] < qs["exit"]


def test_failing_independent_stage_is_isolated_under_concurrency(tmp_path, monkeypatch):
    src = _tiny_wav(tmp_path / "a.wav")

    def boom(*a, **k):
        raise RuntimeError("stage exploded")

    _patch_all_stages(monkeypatch, boom, lambda *a, **k: {"stage": "signal_integrity"})

    out = fi._run_intake(src, "", "", True, False, "none", {}, {})

    # The broken stage is marked failed; every sibling still ran and the bundle
    # completed (no aborted run).
    assert out["stage_status"]["quick_stats"]["ok"] is False
    assert "stage exploded" in out["stage_status"]["quick_stats"]["error"]
    assert out["stage_status"]["signal_integrity"]["ok"] is True
    assert out["stage_status"]["pan_balance"]["ok"] is True
    assert out["stage_status"]["artifacts"]["ok"] is True
    assert "quick_stats" not in out["results"]           # failed -> no result
    assert out["results"]["signal_integrity"] == {"stage": "signal_integrity"}


def test_stage_status_is_declared_order_despite_concurrent_completion(tmp_path, monkeypatch):
    src = _tiny_wav(tmp_path / "a.wav")
    # Make quick_stats finish LAST so completion order != declared order.
    def slow_qs(*a, **k):
        time.sleep(0.2)
        return {"stage": "quick_stats"}
    _patch_all_stages(monkeypatch, slow_qs, lambda *a, **k: {"stage": "signal_integrity"})

    out = fi._run_intake(src, "", "", True, False, "none", {}, {})
    keys = list(out["stage_status"].keys())
    assert keys == out["stages"]                          # declared order preserved
