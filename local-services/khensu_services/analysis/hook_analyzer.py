from __future__ import annotations
from collections import Counter

import numpy as np
from khensu_services.analysis import _lean_spectral as _lean


# ---------------------------------------------------------------------------
# Melodic contour helpers
# ---------------------------------------------------------------------------

def _melodic_contour(start_sec: float, end_sec: float, pitch_notes: list[dict]) -> list[int]:
    """Return the pitch direction sequence for notes in [start_sec, end_sec).

    Each element is +1 (ascending), -1 (descending), or 0 (same pitch).
    An empty list means too few notes to form a contour.
    """
    notes = sorted(
        [n for n in pitch_notes if start_sec <= n["start_sec"] < end_sec],
        key=lambda n: n["start_sec"],
    )
    if len(notes) < 2:
        return []
    return [_step_direction(a, b) for a, b in zip(notes[:-1], notes[1:])]


def pitch_class_interval(a: int, b: int) -> int:
    """Shortest signed pitch-class interval from ``a`` to ``b``, in -6..5."""
    return (int(b) - int(a) + 6) % 12 - 6


def _step_direction(a: dict, b: dict) -> int:
    """+1 / -1 / 0 for the melodic step from note ``a`` to note ``b``.

    Octave-less notes (the chroma fallback writes every note at 60 + pitch
    class and flags it ``pitch_class_only``) take the shortest signed
    pitch-class interval, so B -> C reads as up a semitone rather than down
    eleven (AUDIT-051); real transcriptions keep their actual interval."""
    if a.get("pitch_class_only") or b.get("pitch_class_only"):
        return int(np.sign(pitch_class_interval(a["midi_pitch"], b["midi_pitch"])))
    return int(np.sign(int(b["midi_pitch"]) - int(a["midi_pitch"])))


def _contour_similarity(c1: list[int], c2: list[int]) -> float:
    """Fraction of matching direction steps between two melodic contours.

    Compares only the first min(len(c1), len(c2)) steps. Returns 0 if either
    contour is empty.
    """
    if not c1 or not c2:
        return 0.0
    min_len = min(len(c1), len(c2))
    matches = sum(1 for a, b in zip(c1[:min_len], c2[:min_len]) if a == b)
    return matches / min_len


# ---------------------------------------------------------------------------
# Hook detection
# ---------------------------------------------------------------------------

def detect_hooks(
    y: np.ndarray,
    sr: int,
    sections: list[dict],
    pitch_notes: list[dict] | None,
    bpm: float,
) -> dict:
    """Detect recurring musical hooks using section structure, RMS energy, and
    melodic contour similarity.

    Improvements over the original implementation:
    - Melodic contour similarity is computed between recurring section pairs.
      If two occurrences share ≥ 60% of their pitch direction sequence, the
      hook is classified as melodic ("topline" or "compound") even when the
      average melodic salience is below the previous hard threshold.
    - melodic_salience is boosted by contour_similarity so that the advanced
      path (with basic-pitch notes) produces more accurate hook types.
    """
    if not sections:
        return {"hook_instances": [], "hook_strength": 0.0, "hook_density": 0.0, "hook_count": 0}

    pitch_notes = pitch_notes or []
    duration_sec = float(len(y)) / sr

    boundaries: list[tuple[float, float, str]] = []
    for i, sec in enumerate(sections):
        end = sections[i + 1]["start_sec"] if i + 1 < len(sections) else duration_sec
        boundaries.append((sec["start_sec"], end, sec["label"]))

    label_counts = Counter(s["label"] for s in sections)
    recurring = {lbl for lbl, cnt in label_counts.items() if cnt >= 2}

    if not recurring:
        return {"hook_instances": [], "hook_strength": 0.0, "hook_density": 0.0, "hook_count": 0}

    hop_length = 512
    hop_sec    = hop_length / sr
    rms_frames = _lean.rms(y, hop_length=hop_length)[0]

    def _section_rms(start_sec: float, end_sec: float) -> float:
        s = int(start_sec / hop_sec)
        e = min(int(end_sec / hop_sec), len(rms_frames))
        chunk = rms_frames[s : max(s + 1, e)]
        return float(np.mean(chunk)) if chunk.size > 0 else 0.0

    def _melodic_salience(start_sec: float, end_sec: float) -> float:
        notes_in = [n for n in pitch_notes if start_sec <= n["start_sec"] < end_sec]
        if not notes_in:
            return 0.0
        density  = len(notes_in) / max(end_sec - start_sec, 0.1)
        mean_vel = float(np.mean([n["velocity"] for n in notes_in]))
        return min(density, 1.0) * mean_vel

    # Pre-compute contours per section boundary for fast cross-comparison.
    contours: list[list[int]] = [
        _melodic_contour(s, e, pitch_notes) for s, e, _ in boundaries
    ]

    # For each label, find the best cross-occurrence contour similarity.
    # similarity_by_label[label] = max contour similarity across all pairs
    similarity_by_label: dict[str, float] = {}
    label_to_indices: dict[str, list[int]] = {}
    for idx, (_, _, lbl) in enumerate(boundaries):
        label_to_indices.setdefault(lbl, []).append(idx)

    for lbl, indices in label_to_indices.items():
        if len(indices) < 2:
            continue
        best_sim = 0.0
        for i in range(len(indices)):
            for j in range(i + 1, len(indices)):
                sim = _contour_similarity(contours[indices[i]], contours[indices[j]])
                best_sim = max(best_sim, sim)
        similarity_by_label[lbl] = best_sim

    section_rms_vals = [_section_rms(s, e) for s, e, _ in boundaries]
    max_rms = max(section_rms_vals) if section_rms_vals else 1.0

    instances: list[dict] = []
    for (start_sec, end_sec, label), rms_val in zip(boundaries, section_rms_vals):
        if label not in recurring:
            continue

        salience  = _melodic_salience(start_sec, end_sec)
        norm_rms  = rms_val / max_rms if max_rms > 0 else 0.0
        # Contour similarity boosts melodic salience when sections share pitch direction.
        contour_sim   = similarity_by_label.get(label, 0.0)
        boosted_sal   = float(np.clip(salience + contour_sim * 0.3, 0.0, 1.0))

        strength = float(np.clip(0.6 * boosted_sal + 0.4 * norm_rms, 0.0, 1.0))

        # Hook type: compound > topline > structural, using boosted salience.
        if boosted_sal > 0.4 and norm_rms >= 0.75:
            hook_type = "compound"
        elif boosted_sal > 0.3 or contour_sim >= 0.6:
            hook_type = "topline"
        else:
            hook_type = "structural"

        instances.append({
            "start_sec":  float(start_sec),
            "end_sec":    float(end_sec),
            "hook_type":  hook_type,
            "strength":   strength,
        })

    if not instances:
        return {"hook_instances": [], "hook_strength": 0.0, "hook_density": 0.0, "hook_count": 0}

    top = sorted(instances, key=lambda x: x["strength"], reverse=True)[:3]
    hook_strength = float(np.mean([x["strength"] for x in top]))
    hook_density  = len(instances) / max(duration_sec / 60.0, 1.0 / 60.0)

    return {
        "hook_instances": instances,
        "hook_strength":  float(hook_strength),
        "hook_density":   float(hook_density),
        "hook_count":     len(instances),
    }
