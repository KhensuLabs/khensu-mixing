from __future__ import annotations
from collections import Counter

import numpy as np

_COMMON_PATTERNS = {"ABABC", "AABAB", "AAAB", "ABAB", "AABB", "ABCB", "ABAC", "ABABA"}


def score_memorability(hook_result: dict, sections: list[dict], bpm: float) -> dict:
    hook_strength: float = hook_result.get("hook_strength", 0.0)
    instances: list[dict] = hook_result.get("hook_instances", [])

    melodic = [h for h in instances if h["hook_type"] in ("topline", "compound")]
    melody_salience = float(np.mean([h["strength"] for h in melodic])) if melodic else 0.0
    earworm_likelihood = float(np.clip(hook_strength * melody_salience, 0.0, 1.0))

    labels = [s["label"] for s in sections]
    sequence = "".join(labels)
    has_recurrence = any(cnt >= 2 for cnt in Counter(labels).values())
    if sequence in _COMMON_PATTERNS or any(sequence.startswith(p) for p in _COMMON_PATTERNS):
        structural_predictability = 1.0
    elif has_recurrence:
        structural_predictability = 0.6
    else:
        structural_predictability = 0.3

    # Soft Gaussian BPM alignment peaked at 120 BPM with σ=40.
    # Replaces the hard 90–135/70–160/else cutoffs that unfairly penalised
    # DnB (170+), hip-hop ballads (65–80), and metal (200+).
    # 120 BPM → 1.0 | 80 BPM → 0.61 | 60 BPM → 0.32 | 170 BPM → 0.46
    # Octave-fold first so a half/double-time tempo read isn't penalised.
    from khensu_services.analysis._shared_analysis import fold_bpm_to_window

    folded = fold_bpm_to_window(bpm)
    bpm_alignment = float(np.clip(np.exp(-0.5 * ((folded - 120.0) / 40.0) ** 2), 0.1, 1.0))

    memorability_score = float(np.clip(
        0.5 * earworm_likelihood + 0.3 * structural_predictability + 0.2 * bpm_alignment,
        0.0, 1.0,
    ))

    return {
        "memorability_score": memorability_score,
        "earworm_likelihood": earworm_likelihood,
        "melody_salience": float(np.clip(melody_salience, 0.0, 1.0)),
    }
