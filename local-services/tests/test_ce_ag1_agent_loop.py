"""CE-AG-1: agentic mixing with guardrails (copilot/agent_loop.py + router_agent.py)."""
from __future__ import annotations

import json
import os
import sqlite3
import time

import numpy as np
import pytest
import soundfile as sf
from fastapi.testclient import TestClient
from scipy.signal import butter, sosfilt

from khensu_services.copilot import agent_loop as al
from khensu_services.server import app

SR = 44100


def _shaped_noise(rng, n, slope_db_per_oct=-4.0, lo_hz=35.0):
    """Noise whose octave-band energy falls by ``slope`` dB/oct (music-like),
    band-limited below ``lo_hz`` in the spectrum (no filter start-up transient)."""
    alpha = (1.0 - slope_db_per_oct / 3.0103) / 2.0
    spec = np.fft.rfft(rng.standard_normal(n))
    f = np.fft.rfftfreq(n, 1.0 / SR)
    gain = np.zeros_like(f)
    keep = f >= lo_hz
    gain[keep] = f[keep] ** (-alpha)
    return np.fft.irfft(spec * gain, n)


def _muddy_mix(path, seconds=8.0, seed=0):
    """A quiet, music-like stereo bed (-4 dB/oct, mono lows, moderate width)
    with a big build-up at 200-500 Hz: muddy and too quiet, little else."""
    rng = np.random.default_rng(seed)
    n = int(SR * seconds)
    mid = _shaped_noise(rng, n)
    side = _shaped_noise(rng, n, lo_hz=250.0) * 2.0
    mid = mid + sosfilt(butter(2, [200, 500], "band", fs=SR, output="sos"), mid) * 1.5
    t = np.arange(n) / SR
    env = 0.55 + 0.45 * np.abs(np.sin(2 * np.pi * 1.0 * t)) ** 4     # some dynamics
    y = np.stack([mid + side, mid - side]) * env
    y = y / np.max(np.abs(y)) * 0.25
    sf.write(path, y.T, SR)
    return str(path)


@pytest.fixture
def events_db(tmp_path, monkeypatch):
    db = tmp_path / "events.db"
    monkeypatch.setenv("KHENSU_EVENTS_DB", str(db))
    import khensu_services.events.db as edb
    monkeypatch.setattr(edb, "_conn", None)
    yield edb
    if edb._conn is not None:
        edb._conn.close()
    monkeypatch.setattr(edb, "_conn", None)


# --- mapping ------------------------------------------------------------------
def test_chain_mapping_mirrors_the_desktop_recommendation_mapping():
    chain = al.chain_from_suggested_parameters({
        "highpass_hz": 30, "eq_freq_hz": 300, "eq_gain_db": -2, "comp_ratio": 3,
        "deess_threshold_db": -24, "transient_attack_db": 2, "stereo_width": 1.2,
        "increase_gain_db": 3, "comp_makeup_gain_db": 1, "true_peak_ceiling_dbtp": -1})
    names = [e["effect"] for e in chain]
    assert names == ["HighpassFilter", "PeakFilter", "Compressor", "DeEsser", "TransientShaper",
                     "StereoWidener", "Gain", "Limiter"]
    peak = chain[1]["parameters"]
    assert peak == {"cutoff_frequency_hz": 300.0, "gain_db": -2.0, "q": 0.707}
    comp = chain[2]["parameters"]
    assert comp["threshold_db"] == -18.0 and comp["attack_ms"] == 5.0   # desktop defaults
    assert chain[6]["parameters"]["gain_db"] == 4.0                      # increase + makeup
    assert al.chain_from_suggested_parameters({"reverb_wet": 0.3}) == []  # advice-only key


def test_scale_parameters_moves_toward_neutral():
    p = al.scale_parameters({"eq_gain_db": -4, "stereo_width": 1.4, "comp_ratio": 3}, 0.5)
    assert p == {"eq_gain_db": -2.0, "stereo_width": 1.2, "comp_ratio": 2.0}


def test_metric_for_parameters():
    assert al.metric_for_parameters({"eq_freq_hz": 300, "eq_gain_db": -2}) == "mud_rel_db"
    assert al.metric_for_parameters({"eq_freq_hz": 3500, "eq_gain_db": -2}) == "harsh_rel_db"
    assert al.metric_for_parameters({"true_peak_ceiling_dbtp": -1}) == "true_peak_dbtp"
    assert al.metric_for_parameters({"increase_gain_db": 2}) == "lufs"
    assert al.metric_for_parameters({"stereo_width": 1.2}) == "stereo_width"


# --- objective ----------------------------------------------------------------
def _specs():
    return al.apply_targets(None)


def test_judge_keeps_an_improvement_with_no_collateral_damage():
    before = {"mud_rel_db": 13.0, "crest_db": 12.0, "tilt_db_per_oct": -3.0}
    after = {"mud_rel_db": 11.0, "crest_db": 12.2, "tilt_db_per_oct": -3.2}
    keep, kind, reason, comp = al.judge("mud_rel_db", before, after, _specs(), {"ok": True})
    assert keep and kind == "kept" and "closer to range" in reason
    assert any(c["targeted"] for c in comp)


def test_judge_reverts_when_another_metric_gets_worse():
    before = {"mud_rel_db": 13.0, "crest_db": 9.0}
    after = {"mud_rel_db": 10.0, "crest_db": 6.0}          # crest fell 1 dB below its range
    keep, kind, reason, _ = al.judge("mud_rel_db", before, after, _specs(), {"ok": True})
    assert not keep and kind == "worse" and "Crest factor" in reason


def test_judge_reverts_a_guardrail_block_and_a_too_small_move():
    before, after = {"mud_rel_db": 13.0}, {"mud_rel_db": 10.0}
    keep, kind, _, _ = al.judge("mud_rel_db", before, after, _specs(),
                                {"ok": False, "blocks": ["True peak rose"]})
    assert not keep and kind == "guardrail"
    keep, kind, _, _ = al.judge("mud_rel_db", {"mud_rel_db": 13.0}, {"mud_rel_db": 12.9},
                                _specs(), {"ok": True})
    assert not keep and kind == "insufficient"


def test_derive_findings_names_numbers_and_ranges():
    metrics = {"lufs": -24.0, "true_peak_dbtp": -8.0, "crest_db": 12.0, "tilt_db_per_oct": -3.0,
               "stereo_width": 0.3, "mono_low_corr": 0.9, "low_rel_db": 10.0,
               "mud_rel_db": 13.0, "harsh_rel_db": -10.0, "sib_rel_db": -14.0}
    findings = al.derive_findings(metrics, _specs(), target_lufs=-14.0)
    assert [f["metric"] for f in findings] == ["mud_rel_db", "lufs"]   # EQ first, loudness last
    mud = findings[0]
    assert mud["suggested_parameters"]["eq_freq_hz"] == 300.0 and mud["suggested_parameters"]["eq_gain_db"] < 0
    assert "13.0 dB" in mud["reason"] and mud["range"] == [2.0, 10.5]
    loud = findings[1]
    assert loud["suggested_parameters"]["increase_gain_db"] == 10.0
    assert loud["suggested_parameters"]["true_peak_ceiling_dbtp"] == -1.0


# --- the loop -----------------------------------------------------------------
def test_run_agent_keeps_and_reverts_with_a_full_record(tmp_path):
    src = _muddy_mix(tmp_path / "mix.wav")
    from khensu_services.copilot.dsp import apply_pedalboard_chain

    def render_step(source, output, effects):
        apply_pedalboard_chain(source, output, effects, normalize=False)
        # A stand-in guardrail that refuses the deep (full-strength) mud cut.
        deep = any(e["effect"] == "PeakFilter" and e["parameters"]["gain_db"] < -3.0 for e in effects)
        return {"ok": not deep, "blocks": ["test: cut too deep"] if deep else [], "warnings": []}

    progress = []
    kept = []
    out = al.run_agent(src, str(tmp_path), job_tag="t", render_step=render_step, max_steps=8,
                       on_progress=lambda s: progress.append(s["stage"]), on_kept=kept.append)
    steps = {s["metric"]: s for s in out["steps"]}
    mud = steps["mud_rel_db"]
    assert mud["decision"] == "kept" and mud["strength"] == 0.5
    first, second = mud["attempts"]
    assert first["decision"] == "reverted" and "Guardrail blocked" in first["reason"]
    assert not os.path.exists(first["output_path"])                      # reverted render deleted
    assert second["decision"] == "kept" and os.path.isfile(second["output_path"])
    assert mud["after"]["mud_rel_db"] < mud["before"]["mud_rel_db"] - 0.5
    assert any(c["targeted"] and c["metric"] == "mud_rel_db" for c in second["comparison"])
    assert second["matched_gain_db"] is not None                          # compared at matched loudness
    loud = steps["lufs"]
    assert loud["decision"] == "kept"
    assert out["after"]["lufs"] > out["before"]["lufs"] + 6.0
    assert out["after"]["true_peak_dbtp"] <= -0.9
    for s in out["steps"]:
        assert s["reason"] and s["decision"] in ("kept", "reverted", "skipped")
    assert out["final_path"] == out["kept_paths"][-1] and os.path.isfile(out["final_path"])
    assert [k["metric"] for k in kept] == [s["metric"] for s in out["steps"] if s["decision"] == "kept"]
    assert any(p.startswith("step") for p in progress)


def test_run_agent_with_caller_findings_and_cancel(tmp_path):
    src = _muddy_mix(tmp_path / "mix.wav", seconds=4.0)
    calls = {"n": 0}

    def render_step(source, output, effects):
        from khensu_services.copilot.dsp import apply_pedalboard_chain
        apply_pedalboard_chain(source, output, effects, normalize=False)
        calls["n"] += 1
        return {"ok": True}

    out = al.run_agent(src, str(tmp_path), job_tag="c", render_step=render_step, findings=[
        {"id": "rec-1", "what": "Low-mid build-up", "severity": 60,
         "suggested_parameters": {"eq_freq_hz": 320, "eq_gain_db": -3, "eq_q": 1.0}}])
    assert out["steps"][0]["finding_id"] == "rec-1" and out["steps"][0]["metric"] == "mud_rel_db"
    assert out["steps"][0]["decision"] == "kept"
    with pytest.raises(al.AgentCancelled):
        al.run_agent(src, str(tmp_path), job_tag="x", render_step=render_step,
                     should_cancel=lambda: True)


# --- routes -------------------------------------------------------------------
def test_agent_mix_job_runs_end_to_end_through_the_guarded_pipeline(tmp_path, events_db):
    src = _muddy_mix(tmp_path / "song.wav", seconds=6.0)
    client = TestClient(app)
    res = client.post("/agent/mix", json={"file_path": src, "session_id": "sess-1", "max_steps": 4})
    assert res.status_code == 200, res.text
    job_id = res.json()["job_id"]
    deadline = time.time() + 240
    while True:
        st = client.get(f"/agent/mix/{job_id}")
        assert st.status_code == 200, st.text
        body = st.json()
        if body["status"] in ("completed", "failed", "cancelled"):
            break
        assert time.time() < deadline
        time.sleep(0.5)
    assert body["status"] == "completed", body.get("error")
    result = body["result"]
    assert result["kept_count"] >= 1
    assert os.path.dirname(result["final_path"]) == os.path.splitext(src)[0] + "_automix"
    kept = [s for s in result["steps"] if s["decision"] == "kept"]
    for s in kept:
        assert s["attempts"][-1]["guardrail"]["ok"] is True
    events = events_db.list_events("sess-1")
    types = [e["event_type"] for e in events]
    assert types.count("agent_step_kept") == len(kept)
    assert "agent_mix_completed" in types


def test_agent_mix_validation(tmp_path):
    client = TestClient(app)
    assert client.post("/agent/mix", json={"file_path": str(tmp_path / "missing.wav")}).status_code == 404
    src = _muddy_mix(tmp_path / "a.wav", seconds=2.0)
    bad = client.post("/agent/mix", json={"file_path": src, "targets": {"nope": [0, 1]}})
    assert bad.status_code == 422
    bad = client.post("/agent/mix", json={"file_path": src, "findings": [
        {"id": "x", "suggested_parameters": {"reverb_wet": 0.2}}]})
    assert bad.status_code == 422
    outside = client.post("/agent/mix", json={"file_path": src, "output_dir": "Z:/not/allowed"})
    assert outside.status_code == 403
    assert client.get("/agent/mix/unknown").status_code == 404
    assert client.post("/agent/mix/unknown/cancel").json() == {"ok": False, "status": "unknown"}


def test_agent_findings_and_sessions(tmp_path, monkeypatch):
    src = _muddy_mix(tmp_path / "b.wav", seconds=3.0)
    db = tmp_path / "khensu.db"
    conn = sqlite3.connect(db)
    conn.execute("CREATE TABLE sessions (id TEXT PRIMARY KEY, name TEXT, created_at TEXT, "
                 "updated_at TEXT, data_json TEXT)")
    conn.execute("INSERT INTO sessions VALUES (?,?,?,?,?)",
                 ("s1", "Song", "2026-11-01", "2026-11-02", json.dumps({"files": [{"path": src}]})))
    conn.commit()
    conn.close()
    monkeypatch.setenv("KHENSU_SESSIONS_DB", str(db))
    client = TestClient(app)
    sessions = client.get("/agent/sessions").json()["sessions"]
    assert sessions == [{"id": "s1", "name": "Song", "created_at": "2026-11-01",
                         "updated_at": "2026-11-02", "file_paths": [src]}]
    res = client.post("/agent/findings", json={"session_id": "s1"})
    assert res.status_code == 200, res.text
    body = res.json()
    assert body["file_path"] == src and body["session_id"] == "s1"
    assert any(f["metric"] == "mud_rel_db" for f in body["findings"])
    assert set(body["ranges"]) == set(al.METRICS)
    assert client.post("/agent/findings", json={"session_id": "nope"}).status_code == 404
    assert client.post("/agent/findings", json={}).status_code == 422
