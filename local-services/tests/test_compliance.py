# local-services/tests/test_compliance.py
"""Tests for the delivery/compliance sheet."""
from __future__ import annotations

import numpy as np
import soundfile as sf

from khensu_services.report import compliance as comp

SR = 44100


def _write(tmp_path, name, amp=0.3, dc=0.0, clip=False, subtype="PCM_16"):
    t = np.arange(int(SR * 3.0)) / SR
    y = amp * np.sin(2 * np.pi * 220 * t) + dc
    if clip:
        y[:100] = 1.0
    stereo = np.stack([y, y], axis=1).astype(np.float32)
    p = tmp_path / name
    sf.write(str(p), np.clip(stereo, -1.0, 1.0), SR, subtype=subtype)
    return str(p)


def test_clean_master_passes_core_checks(tmp_path):
    p = _write(tmp_path, "clean.wav", amp=0.2)
    sheet = comp.build_compliance_sheet(p)
    assert sheet["checks"]["clipping"]["status"] == "pass"
    assert sheet["checks"]["dc_offset"]["status"] == "pass"
    assert sheet["format"]["bit_depth"] == 16
    assert "overall" in sheet["summary"]


def test_clipping_flagged(tmp_path):
    p = _write(tmp_path, "clip.wav", amp=0.9, clip=True)
    sheet = comp.build_compliance_sheet(p)
    assert sheet["checks"]["clipping"]["status"] == "fail"
    assert sheet["summary"]["overall"] == "fail"


def test_dc_offset_flagged(tmp_path):
    p = _write(tmp_path, "dc.wav", amp=0.2, dc=0.02)
    sheet = comp.build_compliance_sheet(p)
    assert sheet["checks"]["dc_offset"]["status"] in ("warn", "fail")


def test_platforms_and_markdown_present(tmp_path):
    p = _write(tmp_path, "m.wav", amp=0.3)
    sheet = comp.build_compliance_sheet(p)
    ids = {pl["platform_id"] for pl in sheet["platforms"]}
    # Canonical delivery-table ids (CPL-18): "apple" is now "apple_music".
    assert {"spotify", "apple_music", "club", "youtube"} <= ids
    assert sheet["markdown"].startswith("# Delivery compliance")
    assert "Platform normalisation" in sheet["markdown"]


def test_metadata_missing_warns_on_bare_wav(tmp_path):
    p = _write(tmp_path, "notags.wav")
    sheet = comp.build_compliance_sheet(p)
    assert sheet["checks"]["metadata"]["status"] == "warn"


def test_endpoint(client, tmp_path):
    p = _write(tmp_path, "song.wav")
    resp = client.post("/report/compliance", json={"file_path": p})
    assert resp.status_code == 200
    assert "checks" in resp.json() and "platforms" in resp.json()


def test_endpoint_404(client):
    resp = client.post("/report/compliance", json={"file_path": "/nope.wav"})
    assert resp.status_code == 404


def test_pdf_rendered_when_pdf_path_set(client, tmp_path):
    import os
    p = _write(tmp_path, "song.wav")
    pdf = str(tmp_path / "out" / "compliance.pdf")
    resp = client.post("/report/compliance", json={"file_path": p, "pdf_path": pdf})
    assert resp.status_code == 200
    body = resp.json()
    assert body.get("pdf_path") == pdf
    assert os.path.isfile(pdf) and os.path.getsize(pdf) > 0


def test_render_compliance_pdf_directly(tmp_path):
    import os
    from khensu_services.report.compliance import build_compliance_sheet
    from khensu_services.report.compliance_pdf import render_compliance_pdf
    p = _write(tmp_path, "m.wav")
    sheet = build_compliance_sheet(p)
    out = render_compliance_pdf(sheet, str(tmp_path / "c.pdf"))
    assert os.path.isfile(out)
