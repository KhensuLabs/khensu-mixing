"""Deterministic recommendation narrative: text_factory templates + myth guardrail.

One entry point for every consumer that needs to explain a recommendation
without (or before) an LLM: the Copilot ``/copilot/explain`` endpoint (prompt
context + offline fallback) and the HTML/PDF reports.

``text_factory`` templates interpolate measured/target numbers. Callers often
only know the issue code (the C++ engine's recommendations carry text, not the
raw numbers), so :func:`narrative_for` evaluates each template field on its
own and simply omits fields whose inputs are missing instead of failing the
whole explanation.
"""
from __future__ import annotations

from typing import Any

from khensu_services.recommendations import myth_guardrail, text_factory

# Fields worth surfacing beyond what/why/action, in display order.
DETAIL_FIELDS = ("most_likely_cause", "trade_off", "how_to_verify", "alternative_strategies")


def issue_code_from_id(rec_id: str, file_id: str = "") -> str:
    """The C++ engine builds ids as ``<issue_code>_<file_id>``; recover the code.

    Falls back to the longest known template/myth code that prefixes the id."""
    if file_id and rec_id.endswith("_" + file_id):
        return rec_id[: -(len(file_id) + 1)]
    known = set(text_factory.TEMPLATES) | set(myth_guardrail.MYTHS)
    best = ""
    for code in known:
        if (rec_id == code or rec_id.startswith(code + "_")) and len(code) > len(best):
            best = code
    return best or rec_id


def narrative_for(issue_code: str, *, measured_value: float | None = None, norm_value: float | None = None,
                  unit: str = "", severity: float = 50.0, confidence: float = 0.8,
                  genre_name: str = "", bpm: float | None = None, mood: str = "") -> dict[str, Any]:
    """Template narrative for ``issue_code`` plus myth context.

    Returns only the fields that could be rendered, plus ``is_myth_adjacent`` /
    ``myth_clarification`` (always present) and ``has_template``. ``severity``
    is on the 0-100 scale the templates expect."""
    out: dict[str, Any] = {"has_template": issue_code in text_factory.TEMPLATES}
    template = text_factory.TEMPLATES.get(issue_code)
    if template is not None:
        ctx = {
            "issue_code": issue_code,
            "measured_value": measured_value,
            "norm_value": norm_value,
            "unit": unit,
            "severity": float(severity),
            "confidence": float(confidence),
            "genre_name": genre_name or "this genre",
            "bpm": bpm or 0.0,
            "mood": mood or "neutral",
        }
        for key, fn in template.items():
            try:
                value = fn(ctx)
            except (TypeError, ValueError, KeyError, ZeroDivisionError):
                continue  # needs a number the caller didn't supply
            if value:
                out[key] = value
    return myth_guardrail.apply(out, issue_code)


def explanation_text(base: dict[str, str], narrative: dict[str, Any]) -> str:
    """Offline plain-language explanation: the caller's own text first, the
    template narrative filling what it lacks, then any myth context."""
    parts: list[str] = []
    for text in (base.get("what") or narrative.get("what"),
                 narrative.get("most_likely_cause"),
                 base.get("suggested_action") or narrative.get("suggested_action"),
                 base.get("why_helps") or narrative.get("why_helps"),
                 narrative.get("trade_off")):
        if text:
            parts.append(text.strip().rstrip(".") + ".")
    if narrative.get("is_myth_adjacent") and narrative.get("myth_clarification"):
        parts.append("Context: " + narrative["myth_clarification"])
    return " ".join(parts)


def enrich_report_recommendation(rec: Any, genre_name: str = "", bpm: float | None = None) -> dict[str, Any]:
    """Report row data for a ``RecommendationSummary``: its own fields plus myth
    context and any cause / trade-off / verification the caller didn't send."""
    row = rec.model_dump() if hasattr(rec, "model_dump") else dict(rec)
    code = row.get("issue_code") or ""
    if not code:
        row.setdefault("is_myth_adjacent", False)
        row.setdefault("myth_clarification", "")
        return row
    narrative = narrative_for(code, severity=float(row.get("severity") or 0.0), genre_name=genre_name, bpm=bpm)
    for key in ("most_likely_cause", "trade_off", "how_to_verify"):
        if not row.get(key) and isinstance(narrative.get(key), str):
            row[key] = narrative[key]
    row["is_myth_adjacent"] = bool(narrative.get("is_myth_adjacent"))
    row["myth_clarification"] = narrative.get("myth_clarification", "")
    return row
