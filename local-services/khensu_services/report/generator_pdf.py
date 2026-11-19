"""PDF report generator using reportlab (pure Python, no external binary)."""
from __future__ import annotations
import io
import math
import pathlib
from xml.sax.saxutils import escape as _xml_escape
from reportlab.lib.pagesizes import A4
from reportlab.lib import colors
from reportlab.platypus import (
    SimpleDocTemplate, Table, TableStyle, Paragraph, Spacer,
)
from reportlab.lib.styles import getSampleStyleSheet
from reportlab.graphics.shapes import Drawing, Rect, Line, Polygon, String

from ._report_common import (  # noqa: F401 - re-exported
    LOW_ESTIMATE_CONFIDENCE,
    ReportMarkupError,
    estimate_caveat,
    follow_up_rows,
)
from .models import ReportRequest, ReportTheme


def _esc(value: object) -> str:
    """Escape any interpolated string for a ReportLab Paragraph (CPL-08).

    Paragraph text is mini-markup: an unescaped ``<`` raises a paraparser
    ValueError (a session named ``A<B``) and an ``<img src=...>`` would embed an
    arbitrary local - or UNC - file into the PDF.
    """
    return _xml_escape("" if value is None else str(value))


def _with_confidence(value: str, confidence: float | None) -> str:
    return value if confidence is None else f"{value} (confidence {confidence:.0%})"


def _colour(token: str) -> colors.Color:
    """Convert a validated CSS hex token, preserving its optional alpha."""
    value = token.removeprefix("#")
    alpha = int(value[6:8], 16) / 255.0 if len(value) == 8 else 1.0
    return colors.Color(int(value[0:2], 16) / 255.0, int(value[2:4], 16) / 255.0,
                        int(value[4:6], 16) / 255.0, alpha=alpha)


def _opaque_hex(token: str) -> str:
    """ReportLab font tags accept RGB hex, while other elements keep alpha."""
    return token[:7]


def make_spectral_chart(low: float, mid: float, high: float, theme: ReportTheme) -> Drawing:
    d = Drawing(400, 100)
    # Background
    d.add(Rect(0, 0, 400, 100, fillColor=_colour(theme.bg_surface), strokeColor=None, rx=4, ry=4))

    total = low + mid + high
    if total <= 0:
        low, mid, high = 0.33, 0.33, 0.33
        total = 1.0

    l_pct = low / total
    m_pct = mid / total
    h_pct = high / total

    start_x = 20
    bar_width = 360
    bar_y = 30
    bar_h = 24

    w_l = bar_width * l_pct
    w_m = bar_width * m_pct
    w_h = bar_width * h_pct

    # Low/mid/high use the same category accents as the desktop spectrum view.
    d.add(Rect(start_x, bar_y, w_l, bar_h, fillColor=_colour(theme.accent_teal), strokeColor=None))
    d.add(String(start_x + 2, bar_y + 8, f"Low {l_pct*100:.0f}%", fontSize=8,
                 fillColor=_colour(theme.text_primary), fontName="Helvetica-Bold"))

    d.add(Rect(start_x + w_l, bar_y, w_m, bar_h, fillColor=_colour(theme.accent_blue), strokeColor=None))
    d.add(String(start_x + w_l + 2, bar_y + 8, f"Mid {m_pct*100:.0f}%", fontSize=8,
                 fillColor=_colour(theme.text_primary), fontName="Helvetica-Bold"))

    d.add(Rect(start_x + w_l + w_m, bar_y, w_h, bar_h, fillColor=_colour(theme.accent_amber), strokeColor=None))
    d.add(String(start_x + w_l + w_m + 2, bar_y + 8, f"High {h_pct*100:.0f}%", fontSize=8,
                 fillColor=_colour(theme.text_primary), fontName="Helvetica-Bold"))

    # Title
    d.add(String(20, 75, "SPECTRAL ENERGY BALANCE", fontSize=9, fillColor=_colour(theme.text_accent),
                 fontName="Helvetica-Bold"))
    return d


def make_radar_chart(studio: float, earbuds: float, phone: float, car: float, club: float,
                     theme: ReportTheme) -> Drawing:
    d = Drawing(400, 160)
    # Background
    d.add(Rect(0, 0, 400, 160, fillColor=_colour(theme.bg_surface), strokeColor=None, rx=4, ry=4))

    center_x = 200
    center_y = 70
    max_r = 45

    labels = ["Studio", "Earbuds", "Phone", "Car", "Club"]
    vals = [studio, earbuds, phone, car, club]
    angles = [i * 2.0 * math.pi / 5.0 - math.pi / 2.0 for i in range(5)]

    # Draw concentric pentagons
    for step in [0.25, 0.5, 0.75, 1.0]:
        pts = []
        r = max_r * step
        for a in angles:
            pts.append(center_x + r * math.cos(a))
            pts.append(center_y + r * math.sin(a))
        d.add(Polygon(pts, strokeColor=_colour(theme.border_active), strokeWidth=0.5, fillColor=None))

    # Axes
    for i, a in enumerate(angles):
        ax = center_x + max_r * math.cos(a)
        ay = center_y + max_r * math.sin(a)
        d.add(Line(center_x, center_y, ax, ay, strokeColor=_colour(theme.border_active), strokeWidth=0.5))

        lx = center_x + (max_r + 12) * math.cos(a) - 15
        ly = center_y + (max_r + 8) * math.sin(a) - 3
        d.add(String(lx, ly, labels[i], fontSize=8, fillColor=_colour(theme.text_secondary), fontName="Helvetica"))

    # Value footprint
    val_pts = []
    for i, a in enumerate(angles):
        v = min(1.0, max(0.0, vals[i]))
        r = max_r * v
        val_pts.append(center_x + r * math.cos(a))
        val_pts.append(center_y + r * math.sin(a))

    if len(val_pts) >= 10:
        d.add(Polygon(val_pts, fillColor=None, strokeColor=_colour(theme.accent_teal), strokeWidth=1.5))

    d.add(String(20, 135, "TRANSLATION RISK FOOTPRINT", fontSize=9, fillColor=_colour(theme.text_accent),
                 fontName="Helvetica-Bold"))
    return d


def generate_pdf_report(req: ReportRequest) -> str:
    """Build a PDF report and save it to req.output_path. Returns the saved path.

    A ReportLab markup failure (raised by ``Paragraph()`` itself or at build
    time) becomes :class:`ReportMarkupError` so the router answers 422.
    """
    try:
        return _generate_pdf_report(req)
    except ValueError as exc:
        if isinstance(exc, ReportMarkupError):
            raise
        raise ReportMarkupError("The report text could not be laid out as PDF markup.") from exc


def _generate_pdf_report(req: ReportRequest) -> str:
    buf  = io.BytesIO()
    doc  = SimpleDocTemplate(
        buf, pagesize=A4,
        leftMargin=40, rightMargin=40, topMargin=50, bottomMargin=40,
    )
    styles   = getSampleStyleSheet()
    theme = req.theme
    title_style = styles["Heading1"].clone("KhensuTitle")
    title_style.textColor = _colour(theme.text_accent)
    body_style = styles["Normal"].clone("KhensuBody")
    body_style.textColor = _colour(theme.text_primary)
    section_style = styles["Heading2"].clone("KhensuSection")
    section_style.textColor = _colour(theme.text_accent)
    elements = []

    # --- Header ---
    elements.append(
        Paragraph(
            f"<b><font color='{_opaque_hex(theme.text_accent)}'>KHENSU MIXING</font></b>"
            f" — Analysis Report",
            title_style,
        )
    )
    elements.append(
        Paragraph(
            f"Session: <b>{_esc(req.session_name)}</b>  |  File: {_esc(req.file_name)}"
            f"  |  {_esc(req.computed_at[:10])}",
            body_style,
        )
    )
    elements.append(Spacer(1, 14))

    # --- Key metrics table ---
    rows: list[list[str]] = [["Metric", "Value"]]
    for label, value in [
        ("Integrated LUFS",   f"{req.lufs_integrated:.1f} LUFS"),
        ("Loudness Range",    f"{req.lra:.1f} LU"),
        ("True Peak",         f"{req.true_peak_dbtp:.1f} dBTP"),
        ("Dynamic Range",     f"{req.dynamic_range_db:.1f} dB"),
        ("Crest Factor",      f"{req.crest_factor_db:.1f} dB"),
        ("Stereo Width",      f"{req.stereo_width:.2f}"),
        ("BPM",               _with_confidence(f"{req.bpm:.1f}", req.bpm_confidence) if req.bpm else "—"),
        ("Key",               _with_confidence(req.pitch_key, req.key_confidence) if req.pitch_key else "—"),
        ("Genre",             req.genre_detected or "—"),
        ("Hook Strength",     f"{req.hook_strength:.2f}"),
        ("Memorability",      f"{req.memorability_score:.2f}"),
        ("Clipped Frames",    f"{req.quick_stats_clipping_frames}" if req.quick_stats_clipping_frames is not None else "0"),
    ]:
        rows.append([label, value])

    table = Table(rows, colWidths=[200, 240])
    table.setStyle(TableStyle([
        ("BACKGROUND", (0, 0), (-1, 0), _colour(theme.accent_teal)),
        ("TEXTCOLOR",  (0, 0), (-1, 0), _colour(theme.bg_void)),
        ("FONTNAME",   (0, 0), (-1, 0), "Helvetica-Bold"),
        ("FONTSIZE",   (0, 0), (-1, -1), 9),
        ("TEXTCOLOR",  (0, 1), (-1, -1), _colour(theme.text_primary)),
        ("GRID",       (0, 0), (-1, -1), 0.4, _colour(theme.border_active)),
        ("ROWBACKGROUNDS", (0, 1), (-1, -1), [_colour(theme.bg_surface), _colour(theme.bg_raised)]),
        ("TOPPADDING",  (0, 0), (-1, -1), 4),
        ("BOTTOMPADDING", (0, 0), (-1, -1), 4),
    ]))
    elements.append(table)
    caveat = estimate_caveat(req)
    if caveat:
        elements.append(Spacer(1, 6))
        elements.append(Paragraph(f"<i>{_esc(caveat)}</i>", body_style))
    elements.append(Spacer(1, 14))

    # --- Charts ---
    elements.append(Paragraph("Spectral &amp; Environment Footprint", section_style))
    elements.append(make_spectral_chart(req.spectral_balance_low, req.spectral_balance_mid, req.spectral_balance_high, theme))
    elements.append(Spacer(1, 10))
    elements.append(make_radar_chart(req.translation_risk_studio, req.translation_risk_earbuds,
                                     req.translation_risk_phone, req.translation_risk_car,
                                     req.translation_risk_club, theme))
    elements.append(Spacer(1, 14))

    # --- Follow-up scans (WF-14) ---
    scans = follow_up_rows(req.follow_up_scans)
    if scans:
        elements.append(Paragraph("Follow-up Scans", section_style))
        for scan, fields in scans:
            text = "; ".join(f"{_esc(k)}: {_esc(v)}" for k, v in fields)
            elements.append(Paragraph(f"<b>{_esc(scan)}</b> - {text}", body_style))
        elements.append(Spacer(1, 14))

    # --- Recommendations ---
    if req.recommendations:
        elements.append(Paragraph("Recommendations", section_style))
        from khensu_services.recommendations.narrative import enrich_report_recommendation
        for rec in req.recommendations:
            row = enrich_report_recommendation(rec, req.genre_detected or "", req.bpm)
            elements.append(
                Paragraph(
                    f"<b>[{_esc(rec.type)}]</b> {_esc(rec.what)} — <i>{_esc(rec.suggested_action)}</i>",
                    body_style,
                )
            )
            if row.get("is_myth_adjacent") and row.get("myth_clarification"):
                elements.append(Paragraph(f"<i>Context:</i> {_esc(row['myth_clarification'])}", body_style))
            if req.report_mode == "engineer":
                for label, key in (("Likely cause", "most_likely_cause"), ("Trade-off", "trade_off"),
                                   ("How to verify", "how_to_verify")):
                    if row.get(key):
                        elements.append(Paragraph(f"<i>{label}:</i> {_esc(row[key])}", body_style))
            elements.append(Spacer(1, 4))

    # --- Provenance Timeline (Engineer mode only) ---
    if req.report_mode == "engineer" and req.session_id:
        from khensu_services.events.db import list_events
        events = list_events(req.session_id)
        if events:
            elements.append(Spacer(1, 10))
            elements.append(Paragraph("Provenance Timeline", section_style))
            # Create a table for events
            event_rows = [["Time (UTC)", "Event", "Summary"]]
            for ev in reversed(events):
                time_str = ev.get("created_at", "")
                if len(time_str) >= 19:
                    time_str = time_str[11:19]

                event_rows.append([
                    time_str,
                    ev.get("event_type", ""),
                    ev.get("summary", "")
                ])

            ev_table = Table(event_rows, colWidths=[80, 110, 250])
            ev_table.setStyle(TableStyle([
                ("BACKGROUND", (0, 0), (-1, 0), _colour(theme.accent_teal)),
                ("TEXTCOLOR",  (0, 0), (-1, 0), _colour(theme.bg_void)),
                ("FONTNAME",   (0, 0), (-1, 0), "Helvetica-Bold"),
                ("FONTSIZE",   (0, 0), (-1, -1), 8),
                ("TEXTCOLOR",  (0, 1), (-1, -1), _colour(theme.text_primary)),
                ("GRID",       (0, 0), (-1, -1), 0.4, _colour(theme.border_active)),
                ("ROWBACKGROUNDS", (0, 1), (-1, -1), [_colour(theme.bg_surface), _colour(theme.bg_raised)]),
                ("TOPPADDING",  (0, 0), (-1, -1), 4),
                ("BOTTOMPADDING", (0, 0), (-1, -1), 4),
            ]))
            elements.append(ev_table)

    doc.build(elements)

    out_path = pathlib.Path(req.output_path)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_bytes(buf.getvalue())
    return str(out_path)
