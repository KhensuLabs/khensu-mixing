"""Tests for the HTML report generation endpoint."""
import pathlib
from fastapi.testclient import TestClient

import pytest

from khensu_services.server import app

# Output lands in tmp_path with no source file: opt it into the allowed
# output roots (cwd is not one - see common/output_paths.py).
pytestmark = pytest.mark.usefixtures("tmp_output_root")

client = TestClient(app)

_REQ = {
    "session_name": "Test Track",
    "file_name": "test.wav",
    "computed_at": "2026-06-26T12:00:00Z",
    "lufs_integrated": -14.5,
    "lra": 6.2,
    "true_peak_dbtp": -1.2,
    "crest_factor_db": 12.3,
    "dynamic_range_db": 9.1,
    "stereo_width": 0.72,
    "bpm": 128.0,
    "pitch_key": "F# minor",
    "genre_detected": "Electronic",
    "hook_strength": 0.75,
    "hook_count": 3,
    "memorability_score": 0.68,
    "heaviness_distortion": 0.3,
    "recommendations": [
        {
            "type": "Technical",
            "what": "True peak exceeds -1 dBTP",
            "severity": 85.0,
            "suggested_action": "Lower master bus by 2 dB",
        }
    ],
    "output_path": "",  # replaced per-test
}


def test_report_returns_200(tmp_path):
    req = {**_REQ, "output_path": str(tmp_path / "report.html")}
    assert client.post("/report", json=req).status_code == 200


def test_report_saves_html_file(tmp_path):
    out = tmp_path / "report.html"
    client.post("/report", json={**_REQ, "output_path": str(out)})
    assert out.exists()
    content = out.read_text(encoding="utf-8")
    assert "Test Track" in content
    assert "F# minor" in content


def test_report_response_has_saved_path(tmp_path):
    out = tmp_path / "report.html"
    resp = client.post("/report", json={**_REQ, "output_path": str(out)})
    data = resp.json()
    assert "saved_path" in data
    assert pathlib.Path(data["saved_path"]).exists()


def test_report_html_contains_recommendations(tmp_path):
    out = tmp_path / "report.html"
    client.post("/report", json={**_REQ, "output_path": str(out)})
    content = out.read_text(encoding="utf-8")
    assert "True peak exceeds" in content
    assert "Lower master bus by 2 dB" in content


def test_report_uses_the_supplied_theme_snapshot(tmp_path):
    out = tmp_path / "themed.html"
    theme = {
        "bg_base": "#010203",
        "bg_surface": "#040506",
        "accent_teal": "#070809",
        "text_primary": "#0a0b0c",
    }
    resp = client.post("/report", json={**_REQ, "output_path": str(out), "theme": theme})
    assert resp.status_code == 200
    content = out.read_text(encoding="utf-8")
    assert "--bg-base: #010203" in content
    assert "--bg-surface: #040506" in content
    assert "--accent-teal: #070809" in content
    assert "--text-primary: #0a0b0c" in content
    assert "background: var(--bg-base)" in content


def test_report_rejects_invalid_theme_colour(tmp_path):
    resp = client.post("/report", json={
        **_REQ,
        "output_path": str(tmp_path / "invalid.html"),
        "theme": {"bg_base": "not-a-colour"},
    })
    assert resp.status_code == 422


def test_report_renders_without_optional_fields(tmp_path):
    out = tmp_path / "minimal.html"
    req = {
        **_REQ,
        "output_path": str(out),
        "bpm": 0.0,
        "pitch_key": "",
        "genre_detected": "",
        "hook_strength": 0.0,
        "memorability_score": 0.0,
        "recommendations": [],
    }
    resp = client.post("/report", json=req)
    assert resp.status_code == 200
    assert out.exists()


def test_report_with_engineer_mode_contains_provenance_timeline(tmp_path):
    from khensu_services.events.db import append_event
    session_id = "test-session-for-report"
    append_event(session_id, "feedback", summary="User modified bass parameter")

    out = tmp_path / "eng_report.html"
    req = {
        **_REQ,
        "output_path": str(out),
        "session_id": session_id,
        "report_mode": "engineer",
    }
    resp = client.post("/report", json=req)
    assert resp.status_code == 200
    assert out.exists()
    content = out.read_text(encoding="utf-8")
    assert "Provenance Timeline" in content
    assert "User modified bass parameter" in content


def test_report_lists_follow_up_scans(tmp_path):
    # WF-14: the session's follow-up scans reach the report - headline
    # scalars only, nested data stays in the app.
    out = tmp_path / "scans.html"
    scans = {"mono_compat": {"mono_score": 42.5, "worst_band": "side lows", "per_band": {"sub": 0.9}},
             "source_forensics": {"declared_bits": 24, "upsampled_suspected": False}}
    resp = client.post("/report", json={**_REQ, "output_path": str(out), "follow_up_scans": scans})
    assert resp.status_code == 200, resp.text
    html = out.read_text(encoding="utf-8")
    assert "Follow-up Scans" in html and "Mono compat" in html and "42.5" in html
    assert "Upsampled suspected" in html and "side lows" in html
    assert "per_band" not in html and "Per band" not in html


def test_follow_up_rows_skips_nested_and_long_values():
    from khensu_services.report._report_common import follow_up_rows
    rows = follow_up_rows({"x": {"a": 1, "b": [1, 2], "c": "y" * 500, "d": True}, "empty": {"n": {}}})
    assert rows == [("X", [("A", "1"), ("D", "yes")])]
