"""Conversational A/B critique — measurements, not vibes.

Answers "which take is better, and why?" for two versions of a mix by comparing
them at *matched loudness* (so the louder one doesn't just win) on the same
metric set the post-render guardrail uses: dynamics (crest), inter-sample true
peak, hard clipping, mono-bass compatibility, and a presence/intelligibility
proxy. It reports a per-dimension winner with the actual numbers and a grounded
natural-language verdict that names trade-offs rather than declaring a false
single winner.
"""
from __future__ import annotations

from typing import Any

import soundfile as sf

# How much a metric must differ to call a winner (below this = "tie").
_EPS = {
    "crest_db": 0.8,
    "true_peak_dbtp": 0.3,
    "mono_low_corr": 0.05,
    "vocal_presence_proxy": 0.02,
}


def _load_matched(path_a: str, path_b: str):
    """Load both takes and measure them the way the post-render guardrail does
    (AUDIT-005 / CPL-03): the ABSOLUTE metrics (true peak, flat-top clipping)
    on each take exactly as written, the PERCEPTUAL ones (crest, mono bass,
    presence) on B gain-matched to A's integrated loudness (shared BS.1770
    meter, CPL-11). Judging true peak on the matched copy let a hotter,
    limited take that peaks at -1.0 dBTP read as -7 dBTP and win "true-peak
    safety" - and with it the whole comparison."""
    from khensu_services.copilot.render_guardrails import (
        _absolute_metrics, _perceptual_metrics, loudness_match,
    )
    ya, sra = sf.read(path_a, always_2d=True)
    yb, srb = sf.read(path_b, always_2d=True)
    abs_a, abs_b = _absolute_metrics(ya.T, sra), _absolute_metrics(yb.T, srb)
    yb_matched, gain_db = loudness_match(ya, sra, yb, srb)
    ma = {**abs_a, **_perceptual_metrics(ya.T, sra)}
    mb = {**abs_b, **_perceptual_metrics(yb_matched.T, srb)}
    return ma, mb, round(float(gain_db), 2)


def critique_ab(path_a: str, path_b: str, label_a: str = "A", label_b: str = "B") -> dict[str, Any]:
    """Compare two takes at matched loudness. Never raises on the DSP path."""
    ma, mb, match_gain = _load_matched(path_a, path_b)

    # (metric, higher_is_better, human phrasing)
    dims = [
        ("crest_db", True, "dynamics (crest factor)"),
        ("true_peak_dbtp", False, "true-peak safety"),
        ("clip_samples", False, "hard clipping"),
        ("mono_low_corr", True, "mono-bass compatibility"),
        ("vocal_presence_proxy", True, "presence / intelligibility"),
    ]

    comparisons: list[dict[str, Any]] = []
    a_wins = b_wins = 0
    for key, higher_better, phrase in dims:
        av, bv = ma[key], mb[key]
        eps = _EPS.get(key, 0.0)
        if key == "clip_samples":
            winner = label_a if av < bv else label_b if bv < av else "tie"
        elif abs(av - bv) <= eps:
            winner = "tie"
        else:
            a_better = (av > bv) if higher_better else (av < bv)
            winner = label_a if a_better else label_b
        if winner == label_a:
            a_wins += 1
        elif winner == label_b:
            b_wins += 1
        comparisons.append({"dimension": phrase, "metric": key,
                            "a_value": av, "b_value": bv, "winner": winner})

    # Overall verdict: safety/technical dimensions (clipping, true peak, mono)
    # are the tie-breakers; genuine trade-offs are named, not hidden.
    safety_keys = {"clip_samples", "true_peak_dbtp", "mono_low_corr"}
    a_safety = sum(1 for c in comparisons if c["metric"] in safety_keys and c["winner"] == label_a)
    b_safety = sum(1 for c in comparisons if c["metric"] in safety_keys and c["winner"] == label_b)

    if a_wins == b_wins and a_safety == b_safety:
        overall = "tie"
    elif a_safety != b_safety:
        overall = label_a if a_safety > b_safety else label_b
    else:
        overall = label_a if a_wins > b_wins else label_b

    verdict = _phrase(overall, comparisons, label_a, label_b, match_gain)
    return {
        "label_a": label_a, "label_b": label_b,
        "loudness_match_gain_db": match_gain,   # gain applied to B to match A
        "metrics_a": ma, "metrics_b": mb,
        "comparisons": comparisons,
        "overall": overall,
        "verdict": verdict,
    }


def _phrase(overall: str, comparisons: list[dict[str, Any]],
            la: str, lb: str, _match_gain: float) -> str:
    wins_a = [c["dimension"] for c in comparisons if c["winner"] == la]
    wins_b = [c["dimension"] for c in comparisons if c["winner"] == lb]
    parts = []
    if overall == "tie":
        parts.append("At matched loudness they're a wash on measurements.")
    else:
        loser = lb if overall == la else la
        parts.append(f"At matched loudness, {overall} edges out {loser}.")
    if wins_a:
        parts.append(f"{la} leads on {', '.join(wins_a)}.")
    if wins_b:
        parts.append(f"{lb} leads on {', '.join(wins_b)}.")
    if wins_a and wins_b:
        parts.append("It's a trade-off — pick by what matters most for this release.")
    return " ".join(parts)
