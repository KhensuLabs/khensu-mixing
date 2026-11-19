# local-services/tests/test_platform_render.py
"""Tests for the one-click platform-target master render set."""
from __future__ import annotations

import time

import numpy as np
import soundfile as sf
from fastapi.testclient import TestClient

from khensu_services.server import app
from khensu_services.copilot import platform_render
from khensu_services.analysis.signal_integrity import analyze_true_peak

client = TestClient(app)
SR = 44100


def _loud_master(tmp_path, name="master.wav", peak=0.95):
    # A hot, near-full-scale sine "master" so targets have to turn it DOWN.
    t = np.arange(int(SR * 2.0)) / SR
    y = peak * np.sin(2 * np.pi * 220.0 * t)
    stereo = np.stack([y, y], axis=1).astype(np.float32)
    p = tmp_path / name
    sf.write(str(p), stereo, SR)
    return str(p)


def test_render_hits_target_lufs_and_true_peak_ceiling(tmp_path):
    src = _loud_master(tmp_path)
    out = str(tmp_path / "out_spotify.wav")
    res = platform_render.render_platform_master(
        src, out, target_lufs=-14.0, ceiling_dbtp=-1.0)

    # Loudness lands at (or just under) target; true peak never exceeds ceiling.
    assert res["achieved_lufs"] is not None
    assert abs(res["achieved_lufs"] - (-14.0)) <= 1.0
    assert res["achieved_true_peak_dbtp"] <= -1.0 + 1e-6


def test_true_peak_guaranteed_under_ceiling_when_turned_up(tmp_path):
    # A quiet master that must be turned UP toward a loud club target: the
    # limiter + safety scale must still keep true peak under the ceiling.
    src = _loud_master(tmp_path, "quiet.wav", peak=0.05)
    out = str(tmp_path / "out_club.wav")
    res = platform_render.render_platform_master(
        src, out, target_lufs=-9.0, ceiling_dbtp=-1.0)
    assert res["achieved_true_peak_dbtp"] <= -1.0 + 1e-6

    y, sr = sf.read(out, always_2d=True)
    tp = analyze_true_peak(y.T, sr)["true_peak_dbtp"]
    assert tp <= -1.0 + 1e-6


def test_render_set_covers_all_defaults_with_guardrail(tmp_path):
    src = _loud_master(tmp_path)
    results = platform_render.render_platform_set(src, str(tmp_path / "pm"))
    assert {r["platform_id"] for r in results} == {"spotify", "apple", "club", "youtube"}
    for r in results:
        assert r["status"] == "completed"
        assert "guardrail" in r
        assert r["achieved_true_peak_dbtp"] <= r["ceiling_dbtp"] + 1e-6  # each platform's own ceiling


def test_render_set_respects_platform_subset_and_cancel(tmp_path):
    src = _loud_master(tmp_path)
    seen: list[str] = []

    def progress(pid, done, total):
        seen.append(pid)
        return pid == "spotify" or pid == "done"  # cancel after the first

    results = platform_render.render_platform_set(
        src, str(tmp_path / "pm2"), ["spotify", "apple"], progress_cb=progress)
    # Cancelled before apple rendered.
    assert [r["platform_id"] for r in results] == ["spotify"]


def _poll(client, url, timeout=60.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        r = client.get(url)
        assert r.status_code == 200
        if r.json()["status"] in ("completed", "failed", "cancelled"):
            return r.json()
        time.sleep(0.05)
    raise AssertionError("platform masters job did not finish")


def test_platform_masters_endpoint_end_to_end(tmp_path):
    src = _loud_master(tmp_path)
    start = client.post("/copilot/platform_masters/start",
                        json={"file_path": src, "platforms": ["spotify", "apple"]})
    assert start.status_code == 200
    job_id = start.json()["job_id"]
    final = _poll(client, f"/copilot/platform_masters/status/{job_id}")
    assert final["status"] == "completed"
    assert len(final["results"]) == 2
    assert all(r["achieved_true_peak_dbtp"] <= -1.0 + 1e-6 for r in final["results"])


def test_platform_masters_unknown_platform_422(tmp_path):
    src = _loud_master(tmp_path)
    r = client.post("/copilot/platform_masters/start",
                    json={"file_path": src, "platforms": ["myspace"]})
    assert r.status_code == 422


def test_platform_masters_missing_file_404():
    r = client.post("/copilot/platform_masters/start",
                    json={"file_path": "/nope/missing.wav"})
    assert r.status_code == 404


def _drum_loop(tmp_path, name="drums.wav", level=0.3):
    # Kick + hat transients over silence: very high crest, so the club target
    # needs real limiting rather than a static gain.
    rng = np.random.default_rng(5)
    n = int(SR * 4.0)
    y = np.zeros(n)
    t = np.arange(int(0.15 * SR)) / SR
    kick = np.sin(2 * np.pi * 55.0 * t) * np.exp(-t * 25.0)
    hat = rng.standard_normal(int(0.03 * SR)) * np.exp(-np.arange(int(0.03 * SR)) / (0.005 * SR))
    for beat in range(16):
        s = int(beat * 0.25 * SR)
        y[s:s + kick.size] += kick
        h = s + int(0.125 * SR)
        y[h:h + hat.size] += 0.3 * hat
    y = level * y / np.max(np.abs(y))
    p = tmp_path / name
    sf.write(str(p), np.stack([y, 0.95 * y], axis=1).astype(np.float32), SR)
    return str(p)


def test_limited_targets_land_within_tolerance_or_flag(tmp_path):
    # AUDIT-006/034: pre-gain + true-peak limiter must land within 0.5 LU of
    # target with TP under the ceiling, or say explicitly that it could not.
    sources = {"sine": _loud_master(tmp_path, "q.wav", peak=0.05),
               "drums": _drum_loop(tmp_path)}
    for label, src in sources.items():
        for target in (-14.0, -9.0):
            out = str(tmp_path / f"{label}_{int(-target)}.wav")
            res = platform_render.render_platform_master(
                src, out, target_lufs=target, ceiling_dbtp=-1.0)
            assert res["achieved_true_peak_dbtp"] <= -1.0 + 0.1, (label, target, res)
            if res["target_reached"]:
                assert abs(res["achieved_lufs"] - target) <= 0.5, (label, target, res)
            else:
                assert abs(res["loudness_miss_lu"]) > 0.5


def test_unmeasurable_source_fails_loudly(tmp_path):
    # AUDIT-035: NaN loudness must not be treated as "gain 0".
    import pytest
    p = tmp_path / "silent.wav"
    sf.write(str(p), np.zeros((SR, 2), dtype=np.float32), SR)
    with pytest.raises(ValueError):
        platform_render.render_platform_master(
            str(p), str(tmp_path / "o.wav"), target_lufs=-14.0, ceiling_dbtp=-1.0)
    results = platform_render.render_platform_set(str(p), str(tmp_path / "pm3"), ["spotify"])
    assert results[0]["status"] == "failed"


def test_surround_source_renders(tmp_path):
    t = np.arange(int(SR * 2.0)) / SR
    y = np.zeros((t.size, 6))
    y[:, 0] = y[:, 1] = 0.3 * np.sin(2 * np.pi * 220.0 * t)
    y[:, 3] = 0.3 * np.sin(2 * np.pi * 40.0 * t)
    p = tmp_path / "s51.wav"
    sf.write(str(p), y.astype(np.float32), SR)
    res = platform_render.render_platform_master(
        str(p), str(tmp_path / "o51.wav"), target_lufs=-14.0, ceiling_dbtp=-1.0)
    assert res["target_reached"] is True


def test_safety_variants_keep_the_masters_loudness(tmp_path):
    # CUT-9: by default each platform version keeps the master's loudness and
    # is only made safe for the platform's true-peak ceiling.
    src = _loud_master(tmp_path)
    y, sr = sf.read(src, always_2d=True)
    source_lufs = platform_render._integrated_lufs(y, sr)
    results = platform_render.render_platform_set(src, str(tmp_path / "safe"), ["spotify", "apple"])
    for r in results:
        assert r["status"] == "completed"
        assert r["target_lufs"] == round(source_lufs, 6) or abs(r["target_lufs"] - source_lufs) < 1e-6
        assert r["achieved_lufs"] <= source_lufs + 0.1           # never louder than the master
        assert r["achieved_true_peak_dbtp"] <= r["ceiling_dbtp"] + 1e-6
        assert abs(r["achieved_lufs"] - source_lufs) < 3.0         # not normalised to -14 / -16


def test_matching_loudness_reaches_each_platform_target(tmp_path):
    src = _loud_master(tmp_path)
    results = platform_render.render_platform_set(src, str(tmp_path / "match"), ["spotify", "apple"],
                                                  match_loudness=True)
    for r in results:
        assert r["target_lufs"] in (-14.0, -16.0)
        if r["status"] == "completed":
            assert abs(r["achieved_lufs"] - r["target_lufs"]) <= platform_render.TARGET_TOLERANCE_LU
