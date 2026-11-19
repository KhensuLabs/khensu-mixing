# local-services/khensu_services/analysis/genre_provenance.py
"""Genre-target provenance and confirmation gating (2026-07-15 safety audit P1).

Detected genre is a *candidate*, never authority: detection is a hand-tuned
feature-distance ranker (see genre_detector.estimate_genre_blend),
the target catalog is hand-tuned with no documented real-track corpus, and the
bundled benchmark distribution is synthetic. So:

  * every genre/target response should carry honest provenance metadata, and
  * genre-relative *automatic* actions must abstain until the user confirms a
    genre or selects a reference track.

This module centralizes both so the wording defaults to reference-relative and
no caller silently treats a softmax winner as a calibrated prescription.
"""
from __future__ import annotations

from typing import Any

# Static provenance of the shipped genre-target catalog. NOT a measured
# reference corpus — hand-tuned defaults + synthetic bootstrap percentiles.
GENRE_TARGET_PROVENANCE: dict[str, Any] = {
    "target_source": "hand_tuned_defaults",
    "target_provenance": "Author-curated genre_targets.json; no documented real-track corpus.",
    "sample_size": 0,
    "measurement_protocol": "none (heuristic authoring)",
    "benchmark_distribution": "synthetic",
    "is_reference_data": False,
    "note": "Treat as reference-relative guidance, not a measured target. Confirm a "
            "genre or load a reference before applying genre-relative settings.",
}

# A detected genre is only 'confirmed' when the user explicitly confirms it (or
# selects a reference). Detection alone yields a candidate with a soft margin.
_CONFIRM_MARGIN = 0.15   # top vs runner-up relative-rank gap for a "clear" candidate


def genre_confirmation(blend: list[dict], user_confirmed: bool = False) -> dict[str, Any]:
    """Classify the genre estimate. Returns status + the evidence behind it."""
    top_p = float(blend[0]["rank_score"]) if blend else 0.0
    runner_p = float(blend[1]["rank_score"]) if len(blend) > 1 else 0.0
    margin = round(top_p - runner_p, 3)

    if user_confirmed:
        status = "confirmed"
    elif blend and margin >= _CONFIRM_MARGIN:
        status = "clear_candidate"
    elif blend:
        status = "ambiguous_candidate"
    else:
        status = "unknown"

    return {
        "status": status,
        "is_confirmed": user_confirmed,
        "top_rank_score": round(top_p, 3),
        "margin": margin,
        "candidate": blend[0]["genre"] if blend else None,
    }


def genre_relative_abstention_reason(blend: list[dict], user_confirmed: bool = False) -> str | None:
    """The reason a genre-relative AUTOMATIC action should abstain, or None if
    it may proceed. Never abstains once the user has confirmed a genre/reference.
    """
    if user_confirmed:
        return None
    conf = genre_confirmation(blend, user_confirmed=False)
    if conf["status"] in ("unknown", "ambiguous_candidate"):
        return ("Genre is an unconfirmed candidate "
                f"({conf['candidate'] or 'unknown'}, relative rank={conf['top_rank_score']}, "
                f"margin={conf['margin']}). Confirm a genre or load a reference "
                "before applying genre-relative settings.")
    # A clear candidate still isn't user-confirmed: allow display, but automatic
    # genre-target actions should defer to an explicit confirmation.
    return ("Genre target is a hand-tuned default for an unconfirmed candidate "
            f"({conf['candidate']}). Confirm it or load a reference to apply "
            "genre-relative settings automatically.")
