"""Render the Jinja2 HTML report template and save to disk."""
from __future__ import annotations

import pathlib
import re
from typing import Any

from jinja2 import Environment, FileSystemLoader

from khensu_services.recommendations.narrative import enrich_report_recommendation

from ._report_common import estimate_caveat, follow_up_rows
from .models import ReportRequest

_TEMPLATES = pathlib.Path(__file__).parent / "templates"
_env = Environment(loader=FileSystemLoader(str(_TEMPLATES)), autoescape=True)

# C0/C1 controls, DEL and the Unicode line/paragraph separators (AUDIT-023):
# caller-supplied text must not be able to inject line structure into a file
# on disk. A report has no use for them, so each becomes a plain space.
_CONTROL_CHARS = re.compile("[\x00-\x1f\x7f-\x9f\u2028\u2029]")


def strip_control_chars(value: Any, _depth: int = 0) -> Any:
    """``value`` with control characters replaced by spaces in every string,
    recursing through dicts/lists (other values are returned unchanged)."""
    if isinstance(value, str):
        return _CONTROL_CHARS.sub(" ", value)
    if _depth > 8:
        return value
    if isinstance(value, dict):
        return {k: strip_control_chars(v, _depth + 1) for k, v in value.items()}
    if isinstance(value, list):
        return [strip_control_chars(v, _depth + 1) for v in value]
    return value


def generate_html_report(req: ReportRequest) -> str:
    """Render report, write to req.output_path. Returns the absolute saved path."""
    provenance_events = []
    if req.report_mode == "engineer" and req.session_id:
        from khensu_services.events.db import list_events
        provenance_events = list_events(req.session_id)
        # Reverse to chronological order (oldest first)
        provenance_events = list(reversed(provenance_events))

    context = {
        "session_name": req.session_name,
        "file_name": req.file_name,
        "computed_at": req.computed_at,
        "lufs_integrated": req.lufs_integrated,
        "lra": req.lra,
        "true_peak_dbtp": req.true_peak_dbtp,
        "crest_factor_db": req.crest_factor_db,
        "dynamic_range_db": req.dynamic_range_db,
        "stereo_width": req.stereo_width,
        "stereo_correlation": req.stereo_correlation,
        "bpm": req.bpm,
        "pitch_key": req.pitch_key,
        "bpm_confidence": req.bpm_confidence,
        "key_confidence": req.key_confidence,
        "estimate_caveat": estimate_caveat(req),
        "genre_detected": req.genre_detected,
        "hook_strength": req.hook_strength,
        "hook_count": req.hook_count,
        "memorability_score": req.memorability_score,
        "heaviness_distortion": req.heaviness_distortion,
        "spectral_balance_low": req.spectral_balance_low,
        "spectral_balance_mid": req.spectral_balance_mid,
        "spectral_balance_high": req.spectral_balance_high,
        "spectral_centroid_hz": req.spectral_centroid_hz,
        "translation_risk_studio": req.translation_risk_studio,
        "translation_risk_earbuds": req.translation_risk_earbuds,
        "translation_risk_phone": req.translation_risk_phone,
        "translation_risk_car": req.translation_risk_car,
        "translation_risk_club": req.translation_risk_club,
        "harshness_score": req.harshness_score,
        "harshness_peak_hz": req.harshness_peak_hz,
        "resonance_hotspots": req.resonance_hotspots,
        "recommendations": [enrich_report_recommendation(r, req.genre_detected or "", req.bpm)
                            for r in req.recommendations],
        "report_mode": req.report_mode,
        "provenance_events": provenance_events,
        "quick_stats_lufs_integrated": req.quick_stats_lufs_integrated,
        "quick_stats_true_peak": req.quick_stats_true_peak,
        "quick_stats_lra": req.quick_stats_lra,
        "quick_stats_crest": req.quick_stats_crest,
        "quick_stats_clipping_frames": req.quick_stats_clipping_frames,
        "quick_stats_dc_offset": req.quick_stats_dc_offset,
        "theme": req.theme,
        "follow_up_scans": follow_up_rows(req.follow_up_scans),
    }
    html = _env.get_template("report.html.j2").render(**strip_control_chars(context))
    output = pathlib.Path(req.output_path)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(html, encoding="utf-8")
    return str(output.resolve())
