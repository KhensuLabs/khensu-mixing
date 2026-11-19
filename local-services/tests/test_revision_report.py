"""CE-COL-1 / FEAT-9: revision report (what changed / fixed / got worse)."""
from __future__ import annotations

import json
import sqlite3

import numpy as np
import pytest
import soundfile as sf

from khensu_services.collab import db as collab_db


def test_metrics_diff_reports_fixed_and_worse(client):
    a = {"lufs_integrated": -8.0, "true_peak_dbtp": 0.4, "crest_factor_db": 5.0,
         "stereo_correlation": 0.5, "lra": 4.0}
    b = {"lufs_integrated": -10.0, "true_peak_dbtp": -1.2, "crest_factor_db": 7.5,
         "stereo_correlation": -0.2, "lra": 4.1}
    r = client.post("/revision/report", json={"a_metrics": a, "b_metrics": b,
                                              "a_label": "Mix 3", "b_label": "Mix 4"})
    assert r.status_code == 200, r.text
    rep = r.json()
    fixed = {f["code"] for f in rep["fixed"]}
    worse = {f["code"] for f in rep["got_worse"]}
    assert fixed == {"true_peak_over", "over_limited"}
    assert worse == {"phase_cancellation"}
    changed = {c["metric"]: c for c in rep["changed"]}
    assert changed["lufs_integrated"]["delta"] == -2.0
    assert "lra" not in changed                       # 0.1 LU is below the meaningful delta
    assert rep["summary"].startswith("Mix 4 vs Mix 3: 2 fixed")


def test_caller_findings_override_and_severity_scale(client):
    r = client.post("/revision/report", json={
        "a_metrics": {"lufs_integrated": -9}, "b_metrics": {"lufs_integrated": -9},
        "a_findings": [{"code": "low_mid_mud", "severity": 70, "title": "Mud"},
                       {"code": "harsh", "severity": 40}],
        "b_findings": [{"code": "low_mid_mud", "severity": 30, "title": "Mud"},
                       {"code": "harsh", "severity": 60}]})
    rep = r.json()
    assert [f["code"] for f in rep["improved"]] == ["low_mid_mud"]
    assert [f["code"] for f in rep["got_worse"]] == ["harsh"]
    assert rep["got_worse"][0]["after"] == 0.6


def test_two_files(client, tmp_path):
    sr = 22050
    t = np.arange(sr * 2) / sr
    clean = 0.25 * np.sin(2 * np.pi * 220 * t)
    clipped = np.clip(1.6 * np.sin(2 * np.pi * 220 * t), -1.0, 1.0)
    pa, pb = tmp_path / "before.wav", tmp_path / "after.wav"
    sf.write(str(pa), np.stack([clipped, clipped], 1).astype(np.float32), sr)
    sf.write(str(pb), np.stack([clean, clean], 1).astype(np.float32), sr)
    r = client.post("/revision/report", json={"a_path": str(pa), "b_path": str(pb)})
    assert r.status_code == 200, r.text
    rep = r.json()
    assert rep["a_label"] == "before.wav"
    assert "clipping" in {f["code"] for f in rep["fixed"]}
    assert any(c["metric"] == "true_peak_dbtp" and c["delta"] < 0 for c in rep["changed"])


def test_stored_results(client, tmp_path, monkeypatch):
    db = tmp_path / "khensu.db"
    monkeypatch.setenv("KHENSU_COLLAB_DB", str(db))
    conn = sqlite3.connect(str(db))
    conn.execute("CREATE TABLE analysis_results (id TEXT PRIMARY KEY, session_id TEXT, file_id TEXT,"
                 " computed_at TEXT, lufs_integrated REAL, lra REAL, true_peak_dbtp REAL,"
                 " has_clipping INTEGER, data_json TEXT)")
    conn.execute("INSERT INTO analysis_results VALUES ('r1','s','f','2026-11-01',0,0,0,1,?)",
                 (json.dumps({"lufs_integrated": -7.0, "true_peak_dbtp": 0.2}),))
    conn.execute("INSERT INTO analysis_results VALUES ('r2','s','f','2026-11-02',0,0,0,0,?)",
                 (json.dumps({"lufs_integrated": -9.0, "true_peak_dbtp": -1.5}),))
    conn.commit()
    conn.close()
    try:
        rep = client.post("/revision/report", json={"a_result_id": "r1", "b_result_id": "r2"}).json()
        assert {f["code"] for f in rep["fixed"]} == {"true_peak_over", "clipping"}
        assert client.post("/revision/report", json={"a_result_id": "zz", "b_result_id": "r2"}).status_code == 404
    finally:
        collab_db.close()


@pytest.mark.parametrize("body", [{}, {"a_metrics": {"lufs_integrated": -9}}])
def test_each_side_needs_an_input(client, body):
    assert client.post("/revision/report", json=body).status_code == 422


def test_missing_file_is_404(client, tmp_path):
    r = client.post("/revision/report", json={"a_path": str(tmp_path / "x.wav"),
                                              "b_metrics": {"lufs_integrated": -9}})
    assert r.status_code == 404
