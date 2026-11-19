"""Render a delivery/compliance sheet to a one-page PDF (reportlab, no external
binary). Takes the dict from report.compliance.build_compliance_sheet."""
from __future__ import annotations

import os
from typing import Any
from xml.sax.saxutils import escape as _xml_escape

from reportlab.lib import colors
from reportlab.lib.pagesizes import A4
from reportlab.lib.styles import getSampleStyleSheet
from reportlab.lib.units import mm
from reportlab.platypus import (
    SimpleDocTemplate, Table, TableStyle, Paragraph, Spacer,
)

_STATUS_COLOUR = {"pass": colors.HexColor("#2e7d32"),
                  "warn": colors.HexColor("#ef6c00"),
                  "fail": colors.HexColor("#c62828")}
_STATUS_LABEL = {"pass": "PASS", "warn": "WARN", "fail": "FAIL"}


def _esc(value: object) -> str:
    """Escape interpolated text for a ReportLab Paragraph (CPL-08): the
    metadata check's value carries the file's own title/artist tags, so an
    ``<img src=...>`` pointing at a UNC share planted in a tag must not become markup (it
    would make the service open an SMB connection) and a stray ``<`` must
    not raise a paraparser ValueError."""
    return _xml_escape("" if value is None else str(value))


def render_compliance_pdf(sheet: dict[str, Any], out_path: str) -> str:
    """Render the sheet to ``out_path``; a ReportLab markup failure (from
    ``Paragraph()`` or the build) becomes ReportMarkupError -> 422."""
    from .generator_pdf import ReportMarkupError
    try:
        return _render_compliance_pdf(sheet, out_path)
    except ValueError as exc:
        if isinstance(exc, ReportMarkupError):
            raise
        raise ReportMarkupError("The compliance sheet could not be laid out as PDF markup.") from exc


def _render_compliance_pdf(sheet: dict[str, Any], out_path: str) -> str:
    styles = getSampleStyleSheet()
    doc = SimpleDocTemplate(out_path, pagesize=A4,
                            leftMargin=18 * mm, rightMargin=18 * mm,
                            topMargin=16 * mm, bottomMargin=16 * mm,
                            title="Delivery Compliance")
    story: list = []

    overall = sheet.get("summary", {}).get("overall", "pass")
    title = Paragraph(
        f"Delivery Compliance — <b>{_esc(_STATUS_LABEL.get(overall, str(overall).upper()))}</b>",
        styles["Title"])
    story.append(title)
    fmt = sheet.get("format", {})
    lufs = sheet.get("integrated_lufs")
    sub = (f"{os.path.basename(sheet.get('file_path', ''))} · "
           f"{fmt.get('sample_rate', '?')} Hz / {fmt.get('bit_depth', '?')}-bit / "
           f"{fmt.get('channels', '?')} ch · "
           f"{('%.1f LUFS' % lufs) if lufs is not None else 'loudness n/a'}")
    story.append(Paragraph(_esc(sub), styles["Normal"]))
    story.append(Spacer(1, 6 * mm))

    # Checks table.
    rows = [["Check", "Result", "Value", "Note"]]
    style_cmds = [
        ("BACKGROUND", (0, 0), (-1, 0), colors.HexColor("#1e293b")),
        ("TEXTCOLOR", (0, 0), (-1, 0), colors.white),
        ("FONTSIZE", (0, 0), (-1, -1), 8),
        ("GRID", (0, 0), (-1, -1), 0.4, colors.HexColor("#cbd5e1")),
        ("VALIGN", (0, 0), (-1, -1), "TOP"),
        ("ROWBACKGROUNDS", (0, 1), (-1, -1), [colors.white, colors.HexColor("#f1f5f9")]),
    ]
    for i, (name, c) in enumerate(sheet.get("checks", {}).items(), start=1):
        status = c.get("status", "pass")
        rows.append([name, _STATUS_LABEL.get(status, status.upper()),
                     Paragraph(_esc(c.get("value", "")), styles["BodyText"]),
                     Paragraph(_esc(c.get("note", "")), styles["BodyText"])])
        style_cmds.append(("TEXTCOLOR", (1, i), (1, i),
                           _STATUS_COLOUR.get(status, colors.black)))
    tbl = Table(rows, colWidths=[32 * mm, 16 * mm, 26 * mm, None])
    tbl.setStyle(TableStyle(style_cmds))
    story.append(tbl)
    story.append(Spacer(1, 6 * mm))

    # Platform normalisation table.
    story.append(Paragraph("<b>Platform normalisation</b>", styles["Heading3"]))
    prows = [["Platform", "Target LUFS", "Applied gain", "True peak"]]
    for p in sheet.get("platforms", []):
        g = p.get("normalization_gain_db")
        prows.append([p.get("name", ""), str(p.get("target_lufs", "")),
                      (f"{g:+.1f} dB" if g is not None else "n/a"),
                      "OK" if p.get("true_peak_ok") else "over ceiling"])
    ptbl = Table(prows, colWidths=[46 * mm, 28 * mm, 28 * mm, None])
    ptbl.setStyle(TableStyle([
        ("BACKGROUND", (0, 0), (-1, 0), colors.HexColor("#1e293b")),
        ("TEXTCOLOR", (0, 0), (-1, 0), colors.white),
        ("FONTSIZE", (0, 0), (-1, -1), 8),
        ("GRID", (0, 0), (-1, -1), 0.4, colors.HexColor("#cbd5e1")),
        ("ROWBACKGROUNDS", (0, 1), (-1, -1), [colors.white, colors.HexColor("#f1f5f9")]),
    ]))
    story.append(ptbl)

    doc.build(story)
    return out_path
