"""Tests for the PDF report generation endpoint."""
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
    "computed_at": "2026-06-27T12:00:00Z",
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
    "output_path": "",
}


def test_pdf_returns_200(tmp_path):
    req = {**_REQ, "output_path": str(tmp_path / "report.pdf")}
    resp = client.post("/report/pdf", json=req)
    assert resp.status_code == 200


def test_pdf_saved_path_returned(tmp_path):
    out = tmp_path / "report.pdf"
    req = {**_REQ, "output_path": str(out)}
    data = client.post("/report/pdf", json=req).json()
    assert "saved_path" in data


def test_pdf_file_created(tmp_path):
    out = tmp_path / "report.pdf"
    req = {**_REQ, "output_path": str(out)}
    client.post("/report/pdf", json=req)
    assert out.exists() and out.stat().st_size > 0


def test_pdf_file_is_pdf(tmp_path):
    out = tmp_path / "report.pdf"
    req = {**_REQ, "output_path": str(out)}
    client.post("/report/pdf", json=req)
    assert out.read_bytes()[:4] == b"%PDF"


def test_pdf_accepts_the_same_theme_snapshot_as_html(tmp_path):
    out = tmp_path / "themed.pdf"
    req = {**_REQ, "output_path": str(out), "theme": {"bg_surface": "#102030", "accent_teal": "#40e0d0"}}
    resp = client.post("/report/pdf", json=req)
    assert resp.status_code == 200
    assert out.read_bytes()[:4] == b"%PDF"
