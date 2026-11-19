from __future__ import annotations
from collections import Counter

import numpy as np
from khensu_services.analysis import _lean_spectral as _lean


def score_structural_flow(sections: list[dict], y: np.ndarray, sr: int) -> dict:
    if len(sections) <= 1:
        return {"arc_quality": 0.5}

    duration_sec = float(len(y)) / sr
    hop_length = 512
    hop_sec = hop_length / sr
    rms_frames = _lean.rms(y, hop_length=hop_length)[0]

    boundaries = []
    for i, sec in enumerate(sections):
        end = sections[i + 1]["start_sec"] if i + 1 < len(sections) else duration_sec
        boundaries.append((sec["start_sec"], end, sec["label"]))

    def _rms(start_sec: float, end_sec: float) -> float:
        s = int(start_sec / hop_sec)
        e = min(int(end_sec / hop_sec), len(rms_frames))
        chunk = rms_frames[s : max(s + 1, e)]
        return float(np.mean(chunk)) if chunk.size > 0 else 0.0

    section_rms = [(_rms(s, e), lbl) for s, e, lbl in boundaries]

    label_counts = Counter(lbl for _, lbl in section_rms)
    chorus_label = label_counts.most_common(1)[0][0]
    chorus_rms = float(np.mean([r for r, lbl in section_rms if lbl == chorus_label]))

    intro_rms = section_rms[0][0]
    outro_rms = section_rms[-1][0]
    unique_labels = len({lbl for _, lbl in section_rms})

    arc_quality = 1.0
    if unique_labels <= 1:
        arc_quality -= 0.25
    if intro_rms >= chorus_rms:
        arc_quality -= 0.25
    if outro_rms >= chorus_rms:
        arc_quality -= 0.25

    return {"arc_quality": float(np.clip(arc_quality, 0.0, 1.0))}
