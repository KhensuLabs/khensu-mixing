# local-services/khensu_services/copilot/feedback_decoder.py
"""Client Revision Decoder: translates client feedback language ("make it
warmer", "more punch", "the vocal sounds thin") into ranked candidate
parameter changes — because clients describe feelings, and engineers need
moves.

Deterministic keyword/phrase engine (always available, no network): each
matched concept yields 1-3 interpretations ranked by how commonly that
reading is what clients actually mean, each with a concrete effect-chain
suggestion in the same parameter vocabulary the render pipeline already
understands (eq_freq_hz/eq_gain_db/eq_q, comp_*, reverb_*, etc.).

The optional LLM layer can re-rank or add interpretations later; the
heuristic core is the contract.
"""
from __future__ import annotations

import re
from typing import Any

# ---------------------------------------------------------------------------
# Concept table. Each entry: trigger patterns + ranked interpretations.
# Interpretation: {reading, explanation, parameters{}, confidence}
# ---------------------------------------------------------------------------

_CONCEPTS: list[dict[str, Any]] = [
    {
        "concept": "warmer",
        "patterns": [r"\bwarm(er|th)?\b", r"\bcold\b", r"\bsterile\b", r"\bclinical\b"],
        "interpretations": [
            {
                "reading": "More low-mid body",
                "explanation": "The most common meaning: the track feels thin around 200-400 Hz. "
                               "A gentle low-mid shelf adds perceived warmth without mud.",
                "parameters": {"eq_freq_hz": 250.0, "eq_gain_db": 2.0, "eq_q": 0.8},
                "confidence": 0.6,
            },
            {
                "reading": "Soften the top end",
                "explanation": "Sometimes 'cold' is really 'the highs are brittle'. A slight "
                               "high-shelf cut above 8 kHz rounds the tone.",
                "parameters": {"eq_freq_hz": 9000.0, "eq_gain_db": -1.5, "eq_q": 0.7},
                "confidence": 0.25,
            },
            {
                "reading": "Add harmonic saturation",
                "explanation": "Analog-style saturation adds low-order harmonics that read as "
                               "'warm' even with the same EQ balance.",
                "parameters": {"saturation_drive_db": 3.0},
                "confidence": 0.15,
            },
        ],
    },
    {
        "concept": "punchier",
        "patterns": [r"\bpunch(y|ier)?\b", r"\bhit(s)? hard(er)?\b", r"\bslap\b", r"\bknock(s)?\b",
                     r"\bweak\b", r"\blimp\b"],
        "interpretations": [
            {
                "reading": "Let transients through",
                "explanation": "Punch lives in the attack. Slower compressor attack (or a "
                               "transient shaper boost) lets the initial hit through before "
                               "gain reduction clamps down.",
                "parameters": {"comp_attack_ms": 30.0, "comp_release_ms": 120.0,
                               "transient_attack_db": 3.0},
                "confidence": 0.55,
            },
            {
                "reading": "More kick/low-end impact",
                "explanation": "In hip-hop/EDM contexts 'punch' usually means the kick's 60-100 Hz "
                               "fundamental plus its 2-4 kHz beater click.",
                "parameters": {"eq_freq_hz": 80.0, "eq_gain_db": 2.0, "eq_q": 1.2},
                "confidence": 0.3,
            },
            {
                "reading": "Tighter low end",
                "explanation": "A flabby low end reads as weak. A high-pass on non-bass elements "
                               "plus faster bass compression tightens the hit.",
                "parameters": {"comp_ratio": 4.0, "comp_attack_ms": 10.0},
                "confidence": 0.15,
            },
        ],
    },
    {
        "concept": "thin",
        "patterns": [r"\bthin\b", r"\bsmall\b", r"\bno body\b", r"\bhollow\b", r"\btinny\b"],
        "interpretations": [
            {
                "reading": "Missing 100-300 Hz body",
                "explanation": "Thin almost always means the fundamentals region is scooped or "
                               "high-passed too aggressively.",
                "parameters": {"eq_freq_hz": 180.0, "eq_gain_db": 2.5, "eq_q": 0.9},
                "confidence": 0.65,
            },
            {
                "reading": "Needs density from compression",
                "explanation": "A 2:1 glue compressor raises average level and perceived size "
                               "without changing the tonal balance.",
                "parameters": {"comp_ratio": 2.0, "comp_threshold_db": -18.0},
                "confidence": 0.35,
            },
        ],
    },
    {
        "concept": "muddy",
        "patterns": [r"\bmud(dy)?\b", r"\bboom(y|ing)?\b", r"\bcloud(y|ed)?\b", r"\bwool(l)?y\b",
                     r"\bcongested\b", r"\bboxy\b"],
        "interpretations": [
            {
                "reading": "Cut the 200-400 Hz buildup",
                "explanation": "Mud is almost always cumulative 250 Hz-region energy from "
                               "multiple sources. A broad cut clears it.",
                "parameters": {"eq_freq_hz": 300.0, "eq_gain_db": -2.5, "eq_q": 0.9},
                "confidence": 0.6,
            },
            {
                "reading": "High-pass the non-bass elements",
                "explanation": "If everything reaches down to 60 Hz, nothing has room. "
                               "High-passing pads/guitars/vocals un-stacks the low end.",
                "parameters": {"highpass_hz": 100.0},
                "confidence": 0.4,
            },
        ],
    },
    {
        "concept": "harsh",
        "patterns": [r"\bharsh\b", r"\bshrill\b", r"\bpierc(ing|es)\b", r"\bfatigu(ing|e)\b",
                     r"\bhurts\b", r"\bbrittle\b", r"\bicepick\b"],
        "interpretations": [
            {
                "reading": "Tame 2.5-5 kHz",
                "explanation": "Harshness lives in the upper mids. A dynamic cut engages only "
                               "on the loud moments, keeping presence the rest of the time.",
                "parameters": {"eq_freq_hz": 3500.0, "eq_gain_db": -3.0, "eq_q": 1.4},
                "confidence": 0.6,
            },
            {
                "reading": "De-ess the vocal",
                "explanation": "If the harshness tracks the vocal's S sounds, it's sibilance — "
                               "a de-esser is the right tool, not broadband EQ.",
                "parameters": {"deess_threshold_db": -22.0, "deess_freq_hz": 7000.0},
                "confidence": 0.4,
            },
        ],
    },
    {
        "concept": "wider",
        "patterns": [r"\bwid(er|e|th)\b", r"\bbig(ger)? stereo\b", r"\bmono(ish)?\b", r"\bnarrow\b",
                     r"\bspacious\b", r"\broom(ier)?\b"],
        "interpretations": [
            {
                "reading": "Widen sides above the bass",
                "explanation": "Mid/side width boost above ~200 Hz widens safely; widening the "
                               "low end breaks mono compatibility.",
                "parameters": {"stereo_width": 1.25},
                "confidence": 0.55,
            },
            {
                "reading": "Add spatial depth with reverb/delay",
                "explanation": "Sometimes 'wider' means 'more space around the elements' — a "
                               "short plate or stereo delay does that without M/S processing.",
                "parameters": {"reverb_wet": 0.18, "reverb_size": 0.5},
                "confidence": 0.45,
            },
        ],
    },
    {
        "concept": "vocal_forward",
        "patterns": [r"\bvocal(s)?\b[^.!?]{0,24}\b(up|louder|forward|out front|buried|too quiet|lost|behind)\b",
                     r"\bcan'?t hear the (vocal|singer|lyrics|words)\b",
                     r"\bbring (up|out) the vocal(s)?\b"],
        "interpretations": [
            {
                "reading": "Presence-band lift + level",
                "explanation": "A 1-2 dB vocal level ride plus a 3 kHz presence lift brings the "
                               "vocal forward without drowning the track.",
                "parameters": {"increase_gain_db": 1.5, "eq_freq_hz": 3000.0,
                               "eq_gain_db": 1.5, "eq_q": 1.0},
                "confidence": 0.55,
            },
            {
                "reading": "Carve space in the instruments",
                "explanation": "Often the vocal is loud enough but masked — cutting the "
                               "instrument bus at 2-4 kHz clears the window instead of an "
                               "arms race of levels.",
                "parameters": {"eq_freq_hz": 3000.0, "eq_gain_db": -2.0, "eq_q": 1.2},
                "confidence": 0.45,
            },
        ],
    },
    {
        "concept": "louder",
        "patterns": [r"\bloud(er)?\b", r"\bmore volume\b", r"\bcompetitive\b", r"\bcommercial level\b",
                     r"\bslam(med|s)?\b"],
        "interpretations": [
            {
                "reading": "More density, same peak",
                "explanation": "Perceived loudness comes from average level: gentle bus "
                               "compression + saturation raises density before the limiter.",
                "parameters": {"comp_ratio": 2.0, "comp_threshold_db": -16.0,
                               "saturation_drive_db": 2.0},
                "confidence": 0.5,
            },
            {
                "reading": "Push the limiter",
                "explanation": "1-2 dB more limiter input gain — but note streaming "
                               "normalization will undo pure loudness wins above ~-14 LUFS.",
                "parameters": {"limiter_gain_db": 1.5, "true_peak_ceiling_dbtp": -1.0},
                "confidence": 0.5,
            },
        ],
    },
    {
        "concept": "brighter",
        "patterns": [r"\bbright(er|ness)?\b", r"\bdull\b", r"\bdark\b", r"\bmore air\b",
                     r"\bsparkle\b", r"\bcrisp(er)?\b", r"\bshine\b"],
        "interpretations": [
            {
                "reading": "Air-band shelf",
                "explanation": "A high shelf above 10 kHz adds air and sheen without touching "
                               "the harshness-prone upper mids.",
                "parameters": {"eq_freq_hz": 11000.0, "eq_gain_db": 2.0, "eq_q": 0.7},
                "confidence": 0.6,
            },
            {
                "reading": "Cut the mud instead",
                "explanation": "A dull mix is often a muddy mix: cutting 300 Hz reads as "
                               "'brighter' with none of the hiss risk of boosting highs.",
                "parameters": {"eq_freq_hz": 300.0, "eq_gain_db": -2.0, "eq_q": 0.9},
                "confidence": 0.4,
            },
        ],
    },
    {
        "concept": "less_reverb",
        "patterns": [r"\btoo (much )?(reverb|echo|wet)\b", r"\bdry(er|ier)?\b", r"\bcloser\b",
                     r"\bin (my|your) face\b", r"\bintimate\b", r"\bwash(y|ed out)?\b"],
        "interpretations": [
            {
                "reading": "Reduce reverb send / wet level",
                "explanation": "Pulling the wet level down and shortening the tail brings the "
                               "source closer to the listener.",
                "parameters": {"reverb_wet": 0.08, "reverb_size": 0.35},
                "confidence": 0.65,
            },
            {
                "reading": "Pre-delay instead of less reverb",
                "explanation": "30-60 ms of pre-delay keeps the space but detaches it from the "
                               "dry signal, restoring closeness without killing the ambience.",
                "parameters": {"reverb_predelay_ms": 45.0},
                "confidence": 0.35,
            },
        ],
    },
    {
        "concept": "smoother",
        "patterns": [r"\bsmooth(er)?\b", r"\bpolish(ed)?\b", r"\bpro(fessional)? sound(ing)?\b",
                     r"\bglue(d)?\b", r"\bcohesive\b", r"\bfinished\b"],
        "interpretations": [
            {
                "reading": "Bus glue compression",
                "explanation": "2:1 at slow attack/auto release, 1-2 dB of reduction on the mix "
                               "bus — the classic 'finished record' move.",
                "parameters": {"comp_ratio": 2.0, "comp_threshold_db": -14.0,
                               "comp_attack_ms": 30.0},
                "confidence": 0.5,
            },
            {
                "reading": "Tame the rough edges",
                "explanation": "De-essing plus a gentle dynamic cut at 3-4 kHz removes the "
                               "moments that read as 'unpolished'.",
                "parameters": {"deess_threshold_db": -24.0, "eq_freq_hz": 3500.0,
                               "eq_gain_db": -1.5, "eq_q": 1.3},
                "confidence": 0.5,
            },
        ],
    },
    {
        "concept": "more_energy",
        "patterns": [r"\b(more )?energy\b", r"\bexcit(ing|ement)\b", r"\bflat\b", r"\bboring\b",
                     r"\blifeless\b", r"\bdynamic(s)?\b"],
        "interpretations": [
            {
                "reading": "Restore dynamic contrast",
                "explanation": "If the mix is over-compressed, the chorus can't lift. Easing bus "
                               "compression and automating a 1 dB chorus lift restores motion.",
                "parameters": {"comp_ratio": 1.5, "increase_gain_db": 1.0},
                "confidence": 0.5,
            },
            {
                "reading": "Brighten + saturate the top",
                "explanation": "Excitement often reads as top-end harmonics: an exciter or "
                               "high-shelf plus saturation adds perceived energy.",
                "parameters": {"eq_freq_hz": 10000.0, "eq_gain_db": 1.5,
                               "saturation_drive_db": 2.0},
                "confidence": 0.5,
            },
        ],
    },
]


def decode_feedback(text: str) -> dict[str, Any]:
    """Match client feedback text against the concept table.

    Returns {matches: [{concept, matched_text, interpretations[]}],
             unmatched: bool} with interpretations pre-sorted by confidence.
    """
    lowered = text.lower()
    matches: list[dict[str, Any]] = []
    for entry in _CONCEPTS:
        for pat in entry["patterns"]:
            m = re.search(pat, lowered)
            if m:
                interps = sorted(entry["interpretations"],
                                 key=lambda i: -i["confidence"])
                matches.append({
                    "concept": entry["concept"],
                    "matched_text": m.group(0),
                    "interpretations": interps,
                })
                break

    return {
        "feedback_text": text,
        "matches": matches,
        "unmatched": len(matches) == 0,
    }
