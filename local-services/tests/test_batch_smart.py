# local-services/tests/test_batch_smart.py
"""Per-file smart batch (ENH-4) and mastering styles (ENH-5)."""
from __future__ import annotations

import os
import time

import numpy as np
import soundfile as sf
from fastapi.testclient import TestClient
from scipy.signal import butter, sosfilt

from khensu_services.copilot import batch_smart
from khensu_services.copilot.mastering_agent import (
    crest_target_db, normalize_style, run_mastering_agent, style_seed_stages, style_summary,
    styled_genre,
)
from khensu_services.server import app

SR = 44100
_GENRE = {"id": "test", "lufs_target": -12.0, "spectral_centroid_hz": 2500.0,
          "crest_factor_target_db": 10.0, "true_peak_max": -1.0}


def _shaped_noise(rng, n, lo_hz=20.0):
    x = rng.standard_normal(n)
    sos = butter(2, [lo_hz, 12000.0], "band", fs=SR, output="sos")
    return sosfilt(sos, x)


def _muddy_mix(path, seconds=4.0, seed=0):
    rng = np.random.default_rng(seed)
    n = int(SR * seconds)
    mid = _shaped_noise(rng, n)
    side = _shaped_noise(rng, n, lo_hz=250.0) * 2.0
    mid = mid + sosfilt(butter(2, [200, 500], "band", fs=SR, output="sos"), mid) * 1.5
    t = np.arange(n) / SR
    env = 0.55 + 0.45 * np.abs(np.sin(2 * np.pi * 1.0 * t)) ** 4
    y = np.stack([mid + side, mid - side]) * env
    y = y / np.max(np.abs(y)) * 0.25
    sf.write(str(path), y.T, SR)
    return str(path)


# --- styles --------------------------------------------------------------------
def test_balanced_style_leaves_the_genre_untouched():
    assert styled_genre(_GENRE, "balanced", 1.0) is _GENRE
    assert style_seed_stages("balanced", 1.0) == []
    assert normalize_style("nonsense") == "balanced"
    assert normalize_style("LOUD") == "loud"


def test_styles_move_targets_in_their_direction():
    warm = styled_genre(_GENRE, "warm", 1.0)
    assert warm["spectral_centroid_hz"] < _GENRE["spectral_centroid_hz"]
    opened = styled_genre(_GENRE, "open", 1.0)
    assert opened["spectral_centroid_hz"] > _GENRE["spectral_centroid_hz"]
    assert crest_target_db(opened) > crest_target_db(_GENRE)
    loud = styled_genre(_GENRE, "loud", 1.0)
    assert loud["lufs_target"] > _GENRE["lufs_target"]
    assert crest_target_db(loud) < crest_target_db(_GENRE)
    # Strength scales the move; zero strength is no move.
    half = styled_genre(_GENRE, "loud", 0.5)
    assert _GENRE["lufs_target"] < half["lufs_target"] < loud["lufs_target"]
    assert styled_genre(_GENRE, "loud", 0.0) is _GENRE


def test_loud_style_is_capped_and_keeps_the_ceiling():
    hot = {"id": "hot", "lufs_target": -7.0, "true_peak_max": -1.0}
    assert styled_genre(hot, "loud", 1.0)["lufs_target"] <= -6.0
    # A genre already past the cap is never made quieter by "loud".
    hotter = {"id": "x", "lufs_target": -5.0}
    assert styled_genre(hotter, "loud", 1.0)["lufs_target"] == -5.0


def test_style_seeds_and_summary():
    warm = [s["effect"] for s in style_seed_stages("warm", 1.0)]
    assert warm == ["LowShelfFilter", "TapeSaturation"]
    assert [s["effect"] for s in style_seed_stages("open", 1.0)] == ["HighShelfFilter"]
    assert style_seed_stages("loud", 1.0) == []
    assert style_seed_stages("warm", 0.0) == []
    assert style_summary("loud", 1.0).startswith("Loud at 100%")
    assert style_summary("balanced", 0.7).startswith("Balanced")


def test_mastering_agent_reports_its_style_and_targets(tmp_path):
    src = _muddy_mix(tmp_path / "m.wav", seconds=2.0)
    out = run_mastering_agent(src, str(tmp_path / "o"), _GENRE, max_iterations=2,
                              style="warm", strength=1.0)
    assert out["style"] == "warm" and out["strength"] == 1.0
    assert out["targets"]["centroid_hz"] < _GENRE["spectral_centroid_hz"]
    assert out["targets"]["ceiling_dbtp"] == -1.0
    # The warm seed is in the first rendered chain, the limiter last.
    first = out["iterations"][0]["chain"]
    assert first[0]["effect"] == "LowShelfFilter" and first[-1]["effect"] == "Limiter"


def test_mastering_agent_default_is_balanced(tmp_path):
    src = _muddy_mix(tmp_path / "m.wav", seconds=2.0)
    out = run_mastering_agent(src, str(tmp_path / "o"), _GENRE, max_iterations=1)
    assert out["style"] == "balanced"
    assert out["iterations"][0]["chain"] == []          # unchanged: source measured as-is
    assert out["targets"]["lufs"] == _GENRE["lufs_target"]


def test_mastering_route_validates_style(tmp_path):
    src = _muddy_mix(tmp_path / "m.wav", seconds=1.0)
    client = TestClient(app)
    bad = client.post("/copilot/mastering_agent/start", json={"file_path": src, "style": "fizzy"})
    assert bad.status_code == 422
    bad = client.post("/copilot/mastering_agent/start", json={"file_path": src, "strength": 1.5})
    assert bad.status_code == 422


# --- smart batch -----------------------------------------------------------------
def test_unique_output_names():
    used: set[str] = set()
    assert batch_smart.unique_output_name("a", "_smart", used) == "a_smart.wav"
    assert batch_smart.unique_output_name("a", "_smart", used) == "a_smart_2.wav"


def test_fixable_findings_leave_out_advice_only():
    specs = batch_smart.finding_specs(None, -14.0)
    metrics = {"lufs": -30.0, "true_peak_dbtp": -10.0, "crest_db": 12.0,
               "tilt_db_per_oct": -4.0, "stereo_width": 0.3, "mono_low_corr": 0.9,
               "low_rel_db": 10.0, "mud_rel_db": 15.0, "harsh_rel_db": -10.0, "sib_rel_db": -12.0}
    found = batch_smart.fixable_findings(metrics, specs, -14.0, True)
    assert {f["metric"] for f in found} >= {"mud_rel_db", "lufs"}
    no_loud = batch_smart.fixable_findings(metrics, specs, -14.0, False)
    assert "lufs" not in {f["metric"] for f in no_loud}


def _wait(client, job_id, deadline_s=300):
    deadline = time.time() + deadline_s
    while True:
        body = client.get(f"/copilot/batch/smart/status/{job_id}").json()
        if body["status"] in ("completed", "failed", "cancelled"):
            return body
        assert time.time() < deadline
        time.sleep(0.5)


def test_smart_batch_fixes_each_file_on_its_own(tmp_path):
    a = _muddy_mix(tmp_path / "a.wav", seconds=4.0, seed=1)
    b = _muddy_mix(tmp_path / "b.wav", seconds=4.0, seed=2)
    out_dir = tmp_path / "out"
    client = TestClient(app)
    res = client.post("/copilot/batch/smart/start", json={
        "file_paths": [a, b], "output_dir": str(out_dir), "fix": True, "master": False,
        "max_steps": 3})
    assert res.status_code == 200, res.text
    start = res.json()
    assert start["total_files"] == 2 and start["output_dir"] == str(out_dir)
    body = _wait(client, start["job_id"])
    assert body["status"] == "completed", body.get("error")
    assert body["progress"]["done"] == 2
    assert [r["file_path"] for r in body["results"]] == [a, b]
    for r in body["results"]:
        assert r["status"] in ("completed", "unchanged"), r
        assert r["findings_before"] >= 1
        assert r["before_lufs"] is not None
        if r["status"] == "completed":
            assert os.path.isfile(r["output_path"])
            assert r["output_path"].endswith("_smart.wav")
            assert r["findings_cleared"] == len(r["cleared"])
            assert r["after_lufs"] is not None
    # Work folders are cleaned up; only the results remain.
    assert not [n for n in os.listdir(out_dir) if n.startswith(".work_")]


def test_smart_batch_validation(tmp_path):
    client = TestClient(app)
    a = _muddy_mix(tmp_path / "a.wav", seconds=1.0)
    assert client.post("/copilot/batch/smart/start",
                       json={"file_paths": [str(tmp_path / "missing.wav")]}).status_code == 404
    assert client.post("/copilot/batch/smart/start",
                       json={"file_paths": [a], "fix": False, "master": False}).status_code == 422
    assert client.post("/copilot/batch/smart/start",
                       json={"file_paths": [a], "suffix": "../x"}).status_code == 422
    assert client.post("/copilot/batch/smart/start",
                       json={"file_paths": [a], "master": True, "genre_id": "edm"}).status_code == 409
    assert client.get("/copilot/batch/smart/status/unknown").status_code == 404
    assert client.post("/copilot/batch/smart/unknown/cancel").json() == {"ok": False, "status": "unknown"}


def test_smart_batch_refuses_network_paths(tmp_path):
    import pytest
    from fastapi import HTTPException

    from khensu_services.copilot.router_batch_smart import SmartBatchRequest, _collect_files

    client = TestClient(app)
    a = _muddy_mix(tmp_path / "a.wav", seconds=1.0)
    unc = r"\\fileserver\share\song.wav"
    assert client.post("/copilot/batch/smart/start", json={"file_paths": [unc]}).status_code == 403
    assert client.post("/copilot/batch/smart/start",
                       json={"input_dir": r"\\fileserver\share"}).status_code == 403
    assert client.post("/copilot/batch/smart/start",
                       json={"file_paths": [a], "output_dir": "//fileserver/out"}).status_code == 403
    # The route's own check, not only the app-wide guard: nothing is stat-ed first.
    for req in (SmartBatchRequest(file_paths=[unc]), SmartBatchRequest(input_dir=r"\\host\share"),
                SmartBatchRequest(file_paths=[a], output_dir=r"\\host\out")):
        with pytest.raises(HTTPException) as exc:
            _collect_files(req)
        assert exc.value.status_code == 403


def test_smart_batch_validates_macro_overrides(tmp_path):
    client = TestClient(app)
    a = _muddy_mix(tmp_path / "a.wav", seconds=1.0)
    assert client.post("/copilot/batch/smart/start",
                       json={"file_paths": [a], "loudness_offset_db": 9.0}).status_code == 422
    assert client.post("/copilot/batch/smart/start",
                       json={"file_paths": [a], "ceiling_dbtp": 0.5}).status_code == 422
