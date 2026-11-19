"""Audit 2026-11-06 CPL-08 / SVC-09 / CPL-23: PDF markup escaping, report
route status codes, and the flat-top compliance clipping check."""
from __future__ import annotations

import zlib

import numpy as np
import soundfile as sf

from khensu_services.report import compliance as comp

SR = 44100

_REPORT = {
    "session_name": "S", "file_name": "f.wav", "computed_at": "2026-11-06T00:00:00Z",
    "lufs_integrated": -14.0, "lra": 6.0, "true_peak_dbtp": -1.0, "crest_factor_db": 10.0,
    "dynamic_range_db": 8.0, "stereo_width": 0.5, "recommendations": [],
}


def _pdf_text_blob(path) -> bytes:
    """Raw PDF bytes plus every inflated stream, so a test can look for text
    that ReportLab compressed."""
    data = path.read_bytes()
    out = [data]
    idx = 0
    while True:
        s = data.find(b"stream", idx)
        if s < 0:
            break
        start = s + len(b"stream")
        start += 2 if data[start:start + 2] == b"\r\n" else 1
        e = data.find(b"endstream", start)
        if e < 0:
            break
        try:
            out.append(zlib.decompress(data[start:e].rstrip(b"\r\n")))
        except zlib.error:
            pass
        idx = e + len(b"endstream")
    return b"\n".join(out)


def test_pdf_report_escapes_session_and_recommendation_markup(client, tmp_output_root):
    img = tmp_output_root / "secret.jpg"
    out = tmp_output_root / "r.pdf"
    resp = client.post("/report/pdf", json={
        **_REPORT,
        "session_name": "A<B",
        "file_name": f'<img src="{img.as_posix()}"/>',
        "recommendations": [{"type": "x<y", "what": "<b>bold & </i>broken",
                             "severity": 50.0, "suggested_action": "</para>"}],
        "output_path": str(out),
    })
    assert resp.status_code == 200, resp.text
    blob = _pdf_text_blob(out)
    assert b"/Subtype /Image" not in blob          # no image was embedded


def test_compliance_pdf_escapes_tag_markup():
    from reportlab.lib.styles import getSampleStyleSheet  # noqa: F401 - reportlab present
    from khensu_services.report.compliance_pdf import render_compliance_pdf
    import tempfile
    import os
    sheet = {
        "file_path": "C:/x/<img src='//evil/share/a.jpg'/>.wav",
        "format": {"sample_rate": 44100, "bit_depth": 24, "channels": 2},
        "integrated_lufs": -14.0,
        "checks": {"metadata": {"status": "pass",
                                "value": {"title": "<img src=\"//evil/share/a.jpg\"/>",
                                          "artist": "A<B"},
                                "note": "Title/artist present."}},
        "platforms": [],
        "summary": {"overall": "pass", "fail": 0, "warn": 0},
    }
    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, "c.pdf")
        render_compliance_pdf(sheet, p)
        with open(p, "rb") as fh:
            assert b"/Subtype /Image" not in fh.read()


def test_unescaped_markup_becomes_report_markup_error(tmp_path, monkeypatch):
    import pytest
    from khensu_services.report import generator_pdf
    from khensu_services.report.models import ReportRequest
    # Simulate a missed escape: the paraparser ValueError must surface as the
    # typed ReportMarkupError the router maps to 422.
    monkeypatch.setattr(generator_pdf, "_esc", lambda v: str(v))
    req = ReportRequest(**{**_REPORT, "session_name": "A<B", "output_path": str(tmp_path / "r.pdf")})
    with pytest.raises(generator_pdf.ReportMarkupError):
        generator_pdf.generate_pdf_report(req)


def test_pdf_markup_error_is_422_not_500(client, tmp_output_root, monkeypatch):
    from khensu_services.report import router as report_router
    from khensu_services.report.generator_pdf import ReportMarkupError

    def _raise(req):
        raise ReportMarkupError("The report text could not be laid out as PDF markup.")

    monkeypatch.setattr(report_router, "generate_pdf_report", _raise)
    resp = client.post("/report/pdf", json={**_REPORT, "output_path": str(tmp_output_root / "r.pdf")})
    assert resp.status_code == 422, resp.text


def test_report_write_oserror_is_422_and_generic_errors_do_not_leak(client, tmp_output_root, monkeypatch):
    from khensu_services.report import router as report_router

    def _denied(req):
        raise PermissionError(13, "Permission denied", "C:/secret/path.html")

    monkeypatch.setattr(report_router, "generate_html_report", _denied)
    resp = client.post("/report", json={**_REPORT, "output_path": str(tmp_output_root / "r.html")})
    assert resp.status_code == 422
    assert "secret" not in resp.text

    def _boom(req):
        raise RuntimeError("internal secret detail")

    monkeypatch.setattr(report_router, "generate_html_report", _boom)
    from fastapi.testclient import TestClient
    from khensu_services.server import app
    resp = TestClient(app, raise_server_exceptions=False).post(
        "/report", json={**_REPORT, "output_path": str(tmp_output_root / "r.html")})
    assert resp.status_code == 500
    assert "internal secret detail" not in resp.text


def _write(tmp_path, name, y):
    stereo = np.stack([y, y], axis=1).astype(np.float32)
    p = tmp_path / name
    sf.write(str(p), stereo, SR, subtype="FLOAT")
    return str(p)


def test_clean_full_scale_master_is_not_flagged_as_clipped(tmp_path):
    # A sine whose crest sits exactly on 0 dBFS: isolated full-scale samples,
    # no flat-topped runs. The old >= 0.9995 sample test failed this.
    t = np.arange(SR * 3) / SR
    y = np.sin(2 * np.pi * 441.0 * t + 0.123)
    y = y / np.max(np.abs(y))
    sheet = comp.build_compliance_sheet(_write(tmp_path, "fs.wav", y))
    assert sheet["checks"]["clipping"]["status"] == "pass"
    assert sheet["checks"]["clipping"]["value"] == 0


def test_hard_clipped_flat_top_is_flagged(tmp_path):
    t = np.arange(SR * 3) / SR
    y = np.clip(1.5 * np.sin(2 * np.pi * 220.0 * t), -1.0, 1.0)
    sheet = comp.build_compliance_sheet(_write(tmp_path, "clip.wav", y))
    assert sheet["checks"]["clipping"]["status"] == "fail"
    assert sheet["checks"]["clipping"]["value"] > 0
