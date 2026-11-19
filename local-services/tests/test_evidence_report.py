"""CE-TRU-3: the evidence trail export (/report/evidence)."""
from __future__ import annotations

import json


def _req(root, **over):
    body = {
        "session_name": "Song", "file_name": "song.wav", "genre": "pop",
        "output_path": str(root / "evidence.html"), "json_path": str(root / "evidence.json"),
        "recommendations": [
            {"id": "true_peak_over_f", "issue_code": "true_peak_over",
             "what": "True-peak level is 0.4 dBTP", "suggested_action": "Limit to -1 dBTP",
             "severity": 90,
             "evidence": [{"measurement": "True peak", "value": 0.4, "unit": "dBTP", "threshold": -1.0,
                           "comparison": "above", "source": "absolute"}]},
            {"id": "lufs_too_loud_f", "issue_code": "lufs_too_loud",
             "what": "Integrated loudness <script>alert(1)</script>", "severity": 40,
             "evidence": [{"measurement": "Integrated loudness", "value": -6.2, "unit": "LUFS",
                           "threshold": -8.0, "threshold_low": -14.0, "comparison": "outside",
                           "source": "genre_norm"}]},
            {"issue_code": "clipping", "what": "Clipping", "severity": 70, "evidence": []},
            {"issue_code": "harshness", "what": "Harsh upper mids", "severity": 30,
             "freq_lo_hz": 2500, "freq_hi_hz": 5000, "evidence": []},
        ],
    }
    body.update(over)
    return body


def test_evidence_report_html_and_json(client, tmp_output_root, monkeypatch, tmp_path):
    monkeypatch.setenv("KHENSU_CALIBRATION_TABLE", str(tmp_path / "none.json"))
    r = client.post("/report/evidence", json=_req(tmp_output_root))
    assert r.status_code == 200, r.text
    out = r.json()
    assert out["heuristic_count"] == 4 and out["calibrated_count"] == 0
    rec = out["recommendations"][0]
    assert rec["confidence_basis"] == {"kind": "heuristic"}
    assert rec["evidence"][0]["text"] == ("True peak measured 0.40 dBTP, above the -1.00 dBTP limit "
                                          "from an absolute standard.")
    assert out["recommendations"][1]["evidence"][0]["source"] == "genre_norm"
    # The evidence graph links clipping -> harshness (a known cause -> symptom pair).
    assert any(e["relation"] == "causes" for e in out["edges"]) and out["chains"]

    html = (tmp_output_root / "evidence.html").read_text(encoding="utf-8")
    assert "<script>alert(1)</script>" not in html and "&lt;script&gt;" in html
    assert "Genre norm" in html and "Absolute standard" in html and "heuristic" in html
    assert "<script" not in html.replace("&lt;script", "")      # no scripts at all

    doc = json.loads((tmp_output_root / "evidence.json").read_text(encoding="utf-8"))
    assert doc["format"] == "khensu-evidence-trail" and len(doc["recommendations"]) == 4


def test_evidence_report_path_rules(client, tmp_output_root):
    assert client.post("/report/evidence", json=_req(tmp_output_root, output_path=str(tmp_output_root / "x.txt"))).status_code == 422
    assert client.post("/report/evidence", json=_req(tmp_output_root, json_path=str(tmp_output_root / "x.html"))).status_code == 422
