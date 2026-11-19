from __future__ import annotations

import os
from functools import lru_cache
from pathlib import Path
from typing import Any

import numpy as np

# Bump when the energy/mood heuristic thresholds change so results can be
# tied to the exact heuristic version that produced them.
MOOD_ENERGY_HEURISTIC_VERSION = "1.1.0"

# Mood labels are picked from a valence/arousal quadrant (Russell's circumplex
# model). Valence comes from the key's mode ALONE (major = positive, minor =
# negative; brightness is not used); arousal comes from energy + octave-folded
# tempo. This is a coarse heuristic - no claim that it matches any streaming
# service's audio features.
#
# The low-valence / high-arousal quadrant is "intense", not "aggressive"
# (THY-14): most minor-key dance music (minor house, trance, DnB) lands here,
# and calling it aggressive mislabels it. Aggression is a timbral judgement
# (distortion, attack) this mode-and-tempo heuristic cannot make.
_MOODS = {
    (True,  True):  "euphoric",      # high valence, high arousal
    (True,  False): "uplifting",     # high valence, low arousal
    (False, True):  "intense",       # low valence, high arousal
    (False, False): "melancholic",   # low valence, low arousal
}


def estimate_energy(y: np.ndarray, sr: int, bpm: float) -> float:
    """0-100 perceptual energy from loudness, brightness and tempo."""
    from khensu_services.analysis import _lean_spectral as _lean

    rms = float(_lean.rms(y).mean())
    # RMS for typical mixed/mastered audio sits ~0.02-0.35; rescale into 0..1.
    loudness = float(np.clip(rms / 0.3, 0.0, 1.0))

    centroid = float(_lean.spectral_centroid(y, sr).mean())
    brightness = float(np.clip(centroid / 4000.0, 0.0, 1.0))

    # Fold octave errors (a 70 BPM half-time read of a 140 BPM track, or a
    # 240 BPM double-time read) into the 60-200 window before scoring.
    from khensu_services.analysis._shared_analysis import fold_bpm_to_window

    tempo_norm = float(np.clip((fold_bpm_to_window(bpm) - 60.0) / 120.0, 0.0, 1.0))

    energy = 0.5 * loudness + 0.3 * tempo_norm + 0.2 * brightness
    return float(np.clip(energy * 100.0, 0.0, 100.0))


def estimate_mood(key: str, energy_0_100: float, bpm: float) -> str:
    """Map key mode + energy/tempo onto a coarse mood label.

    Valence needs a mode: an unknown/unparseable key is "neutral" rather
    than silently read as minor (melancholic/intense)."""
    from khensu_services.analysis._shared_analysis import fold_bpm_to_window

    text = (key or "").strip().lower()
    if text.endswith("major"):
        is_major = True
    elif text.endswith("minor"):
        is_major = False
    else:
        return "neutral"
    high_arousal = energy_0_100 >= 55.0 or fold_bpm_to_window(bpm) >= 120.0
    return _MOODS[(is_major, high_arousal)]


def estimate_energy_and_mood(y: np.ndarray, sr: int, bpm: float, key: str) -> dict:
    energy = estimate_energy(y, sr, bpm)
    mood = estimate_mood(key, energy, bpm)
    return {"energy": energy, "mood": mood}


# Two consecutive windows count as "flat" when energy moves less than this
# many points (0-100 scale) — small enough that ordinary micro-dynamics
# don't trigger it, big enough to catch a verse/chorus that never actually
# lifts.
_FLAT_ENERGY_DELTA = 10.0


def _mood_tagger_model_path() -> Path | None:
    env = os.environ.get("KHENSU_MOOD_TAGGER_MODEL", "").strip()
    if env and os.path.isfile(env):
        return Path(env)
    # AID-09: the shared models_dir() (local-services/resources/models, the
    # directory model_manifest reports from) - the old parents[3] pointed at
    # the repo root, so the tagger never loaded while the manifest said it
    # was available.
    from khensu_services.common.model_paths import bundled_model

    bundled = bundled_model("mood_tagger.json")
    return bundled if bundled.is_file() else None


@lru_cache(maxsize=1)
def _load_mood_tagger_model() -> dict[str, Any] | None:
    """Lazily-loaded, process-wide cached mood_tagger artifact (pure JSON,
    no ML runtime dependency) — or None if no model is bundled/configured,
    in which case per-window mood_tags is simply omitted."""
    mp = _mood_tagger_model_path()
    if mp is None:
        return None
    from khensu_services.analysis.learned.mood_tagger import load_model
    try:
        return load_model(str(mp))
    except (OSError, ValueError):
        return None


def _predict_window_mood_tags(seg: np.ndarray, sr: int) -> list[str] | None:
    """Per-window mood/theme tags from the trained mood_tagger, as a second,
    independently-sourced signal alongside the energy/mood heuristic (see
    analyze_emotional_arc's docstring). Returns None when the model isn't
    available rather than raising, matching the other learned-model tiers'
    graceful-degradation pattern."""
    model = _load_mood_tagger_model()
    if model is None:
        return None
    from khensu_services.analysis.learned.mood_tagger import extract_mood_features, predict_labels
    try:
        feats = extract_mood_features(seg, sr)
        labels = predict_labels(model, feats)
    except Exception:
        return None
    return sorted(tag for tag, is_on in labels.items() if is_on)


def analyze_emotional_arc(y_mono: np.ndarray, sr: float, bpm: float, key: str,
                           n_windows: int = 8) -> dict:
    """Time-windowed version of estimate_energy_and_mood: runs the same
    energy/mood heuristic per time window instead of once for the whole
    track, so the dashboard can show how the track's energy/mood actually
    moves (or fails to) across its runtime.

    bpm/key are held constant across windows (reusing the whole-track
    estimate) — tempo/key rarely change meaningfully within a track, and
    per-window re-detection of either would be far noisier than reusing the
    already-reliable whole-track value.

    Returns {"points": [...], "flat_sections": [...], "max_energy_delta": ...}
    with points always covering the track (falls back to a single point if
    the track is too short to window meaningfully, matching iter_time_windows'
    <1s-per-window cutoff).
    """
    from khensu_services.analysis.time_windows import iter_time_windows

    points = []
    for i, frac, _start, _end, seg in iter_time_windows(y_mono, sr, n_windows):
        seg_energy = estimate_energy(seg, int(sr), bpm)
        seg_mood = estimate_mood(key, seg_energy, bpm)
        point = {
            "window_index": i,
            "progress_fraction": round(frac, 4),
            "energy_0_100": round(seg_energy, 2),
            "mood": seg_mood,
        }
        mood_tags = _predict_window_mood_tags(seg, int(sr))
        if mood_tags is not None:
            point["mood_tags"] = mood_tags
        points.append(point)

    if not points:
        whole = estimate_energy_and_mood(y_mono, int(sr), bpm, key)
        whole_point = {"window_index": 0, "progress_fraction": 0.0,
                       "energy_0_100": round(whole["energy"], 2), "mood": whole["mood"]}
        mood_tags = _predict_window_mood_tags(y_mono, int(sr))
        if mood_tags is not None:
            whole_point["mood_tags"] = mood_tags
        return {
            "points": [whole_point],
            "flat_sections": [],
            "max_energy_delta": 0.0,
        }

    deltas = [abs(points[i]["energy_0_100"] - points[i - 1]["energy_0_100"])
              for i in range(1, len(points))]
    max_delta = max(deltas) if deltas else 0.0

    # A "flat section" is a maximal run of consecutive windows where every
    # step's delta stays under the threshold — reported as
    # (start_progress_fraction, end_progress_fraction) ranges, and only kept
    # when it spans 2+ windows (a single window can't be "flat" relative to
    # itself).
    flat_sections: list[tuple[float, float]] = []
    run_start = 0
    for i in range(1, len(points) + 1):
        flat_step = i < len(points) and deltas[i - 1] < _FLAT_ENERGY_DELTA
        if not flat_step:
            if i - 1 > run_start:
                flat_sections.append((points[run_start]["progress_fraction"],
                                      points[i - 1]["progress_fraction"]))
            run_start = i

    return {"points": points, "flat_sections": flat_sections, "max_energy_delta": round(max_delta, 2)}
