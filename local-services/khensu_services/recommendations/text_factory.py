from __future__ import annotations
from collections.abc import Callable

from khensu_services.recommendations import (  # noqa: E402
    templates_level,
    templates_tone,
    templates_stereo,
    templates_masking,
    templates_song,
)
from khensu_services.recommendations.templates_level import _tp_severity_context  # noqa: E402,F401

# Template callable receives a context dict containing all FindingIn fields
# plus: genre_name (str), bpm (float), mood (str).
# Each callable returns str, except "alternative_strategies" which returns list[str].
# ENG-6: the templates live in one module per family; this is their union.
TEMPLATES: dict[str, dict[str, Callable[[dict], str | list[str]]]] = {
    **templates_level.TEMPLATES,
    **templates_tone.TEMPLATES,
    **templates_stereo.TEMPLATES,
    **templates_masking.TEMPLATES,
    **templates_song.TEMPLATES,
}

# Codes that are taste/style decisions rather than technical errors, from the one
# code table the C++ engine owns (ENG-7: IssueCodes.h -> issue_codes.py). Used by
# the router to populate RecommendationOut.is_taste_decision. structural_flow_poor
# uses a dynamic id prefix handled in _enrich_recommendation_out.
from khensu_services.recommendations.issue_codes import TASTE_CODES  # noqa: E402,F401


def _fallback(finding: dict) -> dict:
    code = finding.get("issue_code", "unknown")
    return {
        "what": f"Issue detected: {code}.",
        "why_helps": "Addressing this issue may improve the overall quality of the mix.",
        "suggested_action": "Review the flagged area and apply appropriate corrective action.",
        "engineer_note": (
            f"The system flagged '{code}' but no specific template was found. "
            "Treat this as a manual review item."
        ),
        "severity_context": "Severity and confidence values are computed numerically.",
        "confidence_reason": "Confidence reflects the analysis method used for this domain.",
        "most_likely_cause": "Unknown — manual investigation required.",
        "trade_off": "Unable to determine trade-offs without specific template.",
        "bpm_mood_genre_relevance": "Context-specific analysis not available for this issue code.",
        "comparison_to_similar_songs": "No genre comparison available for this issue code.",
        "alternative_strategies": ["Consult documentation for this issue code."],
    }


def enrich(finding: dict, genre_name: str, bpm: float | None, mood: str) -> dict:
    ctx = {**finding, "genre_name": genre_name, "bpm": bpm or 0.0, "mood": mood}
    template = TEMPLATES.get(finding["issue_code"])
    if template is None:
        return _fallback(finding)
    return {key: fn(ctx) for key, fn in template.items()}
