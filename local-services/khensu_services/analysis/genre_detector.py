from __future__ import annotations
import json
import logging
from pathlib import Path

import numpy as np

from khensu_services.analysis._shared_analysis import (
    fold_bpm_to_window,
    octave_folded_bpm_distance,
)

_log = logging.getLogger(__name__)

# Bump this when the BPM/centroid heuristic thresholds are changed so analysis
# results can be tied to the exact genre-detection version that produced them.
GENRE_HEURISTIC_VERSION = "1.0.0"


# Share of power below 120 Hz above which a dark, half-time-read track is
# treated as 808-driven trap rather than lo-fi.
_TRAP_SUB_WEIGHT = 0.45


def estimate_genre_and_subgenre(y: np.ndarray, sr: int, bpm: float) -> tuple[str, str]:
    """Estimate genre and subgenre based on tempo and spectral features.

    The tempo bands below span roughly 60-200 BPM (the genre_targets.json
    tempo convention), so a beat tracker's octave error outside that window
    (a 55 BPM half-time read, a 240 BPM double-time read) is folded back
    into it before the bands are applied.
    """
    bpm = fold_bpm_to_window(bpm, 60.0, 200.0)
    try:
        from khensu_services.analysis import _lean_spectral as _lean

        centroid = float(_lean.spectral_centroid(y, sr).mean())
        rolloff = float(_lean.spectral_rolloff(y, sr).mean())
    except Exception:
        centroid = 2000.0
        rolloff = 3500.0
    try:
        from khensu_services.analysis.dsp_metrics import low_band_energy_ratio
        sub_weight = low_band_energy_ratio(y, sr, 120.0)
    except Exception:
        sub_weight = 0.0

    detected_genre = "pop"
    detected_subgenre = "synthpop"

    if bpm < 95:
        if centroid < 1500:
            detected_genre = "hiphop"
            # Half-time read of a trap beat: the 65-90 BPM band is where
            # trackers report 130-180 BPM trap at half speed. Trap's 808s
            # put far more energy under 120 Hz than lo-fi's soft kicks, so
            # a sub-heavy dark track at a doubled tempo of 130-180 is trap.
            if 130.0 <= 2.0 * bpm <= 180.0 and sub_weight >= _TRAP_SUB_WEIGHT:
                detected_subgenre = "trap"
            else:
                detected_subgenre = "lofi"
        else:
            detected_genre = "rnb"
            detected_subgenre = "contemporary"
    elif bpm < 118:
        if centroid > 2200:
            detected_genre = "pop"
            detected_subgenre = "dancepop"
        else:
            detected_genre = "rock"
            detected_subgenre = "indierock"
    elif bpm < 135:
        if centroid > 2000:
            detected_genre = "electronic"
            detected_subgenre = "house"
        else:
            detected_genre = "pop"
            detected_subgenre = "synthpop"
    else:
        if centroid < 1800:
            detected_genre = "hiphop"
            detected_subgenre = "trap"
        elif rolloff > 3800:
            detected_genre = "rock"
            detected_subgenre = "alternative"
        else:
            detected_genre = "electronic"
            detected_subgenre = "dubstep"

    return detected_genre, detected_subgenre


def genre_blend_for_file(file_path: str | None, y: np.ndarray, sr: int, bpm: float, *,
                         measured_features: dict[str, float] | None = None
                         ) -> tuple[list[dict], dict | None]:
    """The genre candidates for a file: the learned CLAP probe when it can
    run (``learned.genre_probe``), else the feature-distance ranker below.
    Returns ``(blend, probe_prediction_or_None)``; ``blend`` has the
    ``estimate_genre_blend`` shape either way."""
    if file_path:
        from khensu_services.analysis.learned import genre_probe

        pred = genre_probe.predict_file(file_path, personal=False)
        if pred is not None:
            return list(pred["genres"]), pred
    return estimate_genre_blend(y, sr, bpm, measured_features=measured_features), None


# (id, parent, bpm_mid, centroid_hz) anchors for estimate_genre_blend(),
# loaded from the same genre_targets.json the C++ GenreDatabase/GenreEntry
# side reads (khensu/targets/GenreDatabase.h) — one catalog, not two, so
# genre-blend results always name an id the reference-benchmark library and
# blendGenreEntries() can actually resolve. bpm_mid is the midpoint of
# typical_bpm_range; centroid_hz comes straight from the entry. Falls back
# to the small hand-picked table below if the JSON can't be found/parsed
# (e.g. a stripped-down test environment), so estimate_genre_blend never
# hard-fails.
_FALLBACK_ANCHORS: list[tuple[str, str, float, float, float, float]] = [
    ("lo-fi-hip-hop", "hip-hop",    80.0,  1200.0, 0.55, 12.0),
    ("r-and-b",       "r-and-b",    80.0,  2600.0, 0.35, 10.0),
    ("dance-pop",     "pop",       105.0,  2800.0, 0.35,  9.0),
    ("indie-rock",    "rock",      105.0,  1800.0, 0.40, 10.0),
    ("house",         "electronic",125.0,  2600.0, 0.45,  8.0),
    ("synthpop",      "pop",       125.0,  1600.0, 0.35,  9.0),
    ("trap",          "hip-hop",   150.0,  1400.0, 0.60,  7.0),
    ("alt-rock",      "rock",      150.0,  2400.0, 0.40, 10.0),
    ("dubstep",       "electronic",150.0,  2400.0, 0.55,  7.0),
]

# Normalization scales roughly matching each axis's typical spread, so no
# single axis (e.g. centroid_hz's much larger raw range) dominates distance.
_BPM_SCALE, _CENTROID_SCALE = 60.0, 1200.0
# Tempo distance (BPM) added to an octave-folded tempo match in the blend.
_OCTAVE_FOLD_PENALTY_BPM = 8.0

_anchor_cache: list[tuple[str, str, float, float]] | None = None


def _load_genre_anchors() -> list[tuple[str, str, float, float]]:
    global _anchor_cache
    if _anchor_cache is not None:
        return _anchor_cache

    try:
        # local-services/khensu_services/analysis/genre_detector.py ->
        # parents[3] is the repo root (matches copilot/router.py's
        # _lookup_genre, which resolves the same file from one directory
        # over at the same depth).
        targets_path = Path(__file__).resolve().parents[3] / "resources" / "genre_norms" / "genre_targets.json"
        data = json.loads(targets_path.read_text(encoding="utf-8"))
        anchors = []
        for g in data.get("genres", []):
            gid = g.get("id")
            if not gid:
                continue
            bpm_range = g.get("typical_bpm_range") or [90.0, 140.0]
            bpm_mid = (float(bpm_range[0]) + float(bpm_range[1])) / 2.0
            centroid = float(g.get("spectral_centroid_hz", 2500.0))
            low_end = float(g.get("low_end_weight", 0.5))
            crest = float(g.get("crest_factor_target_db",
                              14.0 - float(g.get("compression_ratio_typical", 3.0)) * 1.2))
            anchors.append((gid, str(g.get("parent", gid)), bpm_mid, centroid, low_end, crest))
        if anchors:
            _anchor_cache = anchors
            return _anchor_cache
    except Exception:
        _log.warning("Could not load genre_targets.json for genre-blend anchors; "
                     "falling back to the small built-in table.", exc_info=True)

    _anchor_cache = _FALLBACK_ANCHORS
    return _anchor_cache


def estimate_genre_blend(y: np.ndarray, sr: int, bpm: float,
                          min_rank_score: float = 0.15,
                          measured_features: dict[str, float] | None = None) -> list[dict]:
    """Soft multi-label genre estimate over the full genre_targets.json
    catalog (100+ genres, not just the 9 branches of
    estimate_genre_and_subgenre's decision tree): scores every catalog
    entry by inverse distance (in normalized tempo, tonal, low-end, and
    dynamics feature space) to the track's measured features. Scores are
    relative ranks, not probabilities, and returns entries at or above
    min_rank_score — so a
    track sitting between two genres (e.g. hyperpop-leaning pop at 122 BPM
    with a bright centroid) shows both, and niche/underground genres are
    reachable results, not just the 9 coarse families the older single-label
    heuristic could name.

    Returns a list of {"genre", "subgenre", "rank_score"} dicts sorted by
    rank descending, where "genre" is the specific catalog id (e.g.
    "shoegaze") — resolvable via GenreDatabase::findById and the
    reference-benchmark library — and "subgenre" is that entry's parent
    family (e.g. "alt-rock"), kept for display context. Always contains at
    least one entry (the nearest anchor), even if it's below
    min_rank_score, so callers never get [].
    """
    try:
        from khensu_services.analysis import _lean_spectral as _lean

        centroid = float(_lean.spectral_centroid(y, sr).mean())
    except Exception:
        centroid = 2000.0

    anchors = _load_genre_anchors()
    if measured_features is None:
        from khensu_services.analysis.dsp_metrics import low_band_energy_ratio
        from khensu_services.analysis.loudness import crest_factor_db

        # Shared crest (per channel, -90 dBFS floor; DSP-11); 10 dB - the
        # neutral default used below - for silence.
        crest = crest_factor_db(y)
        measured_features = {
            # Welch PSD, not a full-length rfft of the whole track (memory).
            "low_end_weight": low_band_energy_ratio(y, sr, 250.0),
            "crest_factor_db": float(crest) if crest is not None else 10.0,
        }
    dists = []
    for gid, parent, a_bpm, a_centroid, a_low_end, a_crest in anchors:
        # Octave-folded tempo distance (audit #12): beat trackers routinely
        # report half- or double-time, and a 70 BPM trap beat is the same
        # groove as 140, so take the closest of b, 2b and b/2 - but a folded
        # match pays a penalty, so a genre the tempo matches directly wins
        # over one it only matches an octave away (AUDIT-084).
        bpm_dist = octave_folded_bpm_distance(bpm, a_bpm, _OCTAVE_FOLD_PENALTY_BPM)
        d = np.sqrt(
            (bpm_dist / _BPM_SCALE) ** 2
            + ((centroid - a_centroid) / _CENTROID_SCALE) ** 2
            + 0.35 * ((float(measured_features.get("low_end_weight", 0.5)) - a_low_end) / 0.25) ** 2
            + 0.25 * ((float(measured_features.get("crest_factor_db", 10.0)) - a_crest) / 4.0) ** 2
        )
        dists.append((gid, parent, float(d)))

    # Monotonic inverse distance: a rank only. It deliberately does not sum
    # to one and must never be presented as classifier confidence.
    blend = sorted(
        (
            {"genre": gid, "subgenre": parent, "rank_score": round(1.0 / (1.0 + d), 3)}
            for gid, parent, d in dists
        ),
        key=lambda e: -e["rank_score"],
    )
    # Multiple catalog entries can legitimately tie/cluster near the same
    # bpm+centroid point (e.g. several -pop variants) — cap at the top 5 so
    # a crowded family doesn't produce a dozen near-tied low-probability
    # entries that aren't meaningfully distinguishable from just noise.
    filtered = [e for e in blend if e["rank_score"] >= min_rank_score][:5]
    return filtered if filtered else blend[:1]
