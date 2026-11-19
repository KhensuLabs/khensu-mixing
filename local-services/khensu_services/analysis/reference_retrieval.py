# local-services/khensu_services/analysis/reference_retrieval.py
"""Nearest-reference retrieval (pipeline #8).

`benchmark.py` ranks a track's metrics as a percentile within one genre's
reference distribution. This goes further: it finds the *closest individual
professional reference exemplar(s)* in the library and reports the per-metric
delta to that match — a sharper, concrete target ("2 LU louder and 3 dB less
dynamic than your nearest reference") than a static genre-average norm. It
reuses the same library and metric set as benchmark.py, so real analyzed
references drop in without code change.

Distance is Euclidean in z-scored metric space (normalized by the whole
library's per-metric spread) so LUFS, LRA, tilt, and crest are comparable.
Retrieval can be scoped to a genre or run across the whole library.
"""
from __future__ import annotations

from typing import Any

import numpy as np

from khensu_services.analysis.benchmark import _METRIC_KEYS, _load_library
from khensu_services.analysis.context import load_audio

def _spectral_tilt_db_per_octave(mono: np.ndarray, sr: int) -> float:
    """dB/octave tilt — the shared dsp_metrics definition (fixed octave band
    set capped at min(Nyquist, 20 kHz)), the same metric the library stores."""
    from khensu_services.analysis.dsp_metrics import spectral_tilt_db_per_octave
    return spectral_tilt_db_per_octave(mono, sr)


def _target_profile_from_audio(y: np.ndarray, sr: int) -> dict[str, float]:
    """Build the reference-library metric vector for already-decoded audio."""
    from khensu_services.analysis.loudness import (
        crest_factor_db, integrated_lufs, loudness_range,
    )

    # Mono stays ONE channel for BS.1770 (no duplicate-to-stereo +3 LU).
    mono = y.mean(axis=0) if y.ndim > 1 else y

    lufs = integrated_lufs(y, sr)
    lra = loudness_range(y, sr)
    crest = crest_factor_db(y)   # per channel, max (see loudness.py)
    # The distance metric needs numbers: silence/too-short fall back to the
    # BS.1770 absolute-gate floor, zero range and zero crest.
    return {
        "lufs_integrated": round(lufs, 2) if lufs is not None else -70.0,
        "lra": round(lra, 2) if lra is not None else 0.0,
        "spectral_tilt_db_per_octave": round(_spectral_tilt_db_per_octave(mono, sr), 2),
        "crest_factor_db": round(crest, 2) if crest is not None else 0.0,
    }


def target_profile(file_path: str, ctx=None) -> dict[str, float]:
    """The track's {lufs_integrated, lra, spectral_tilt_db_per_octave,
    crest_factor_db} — the same 4-metric vector the reference library uses."""

    y, sr = ctx.load(sr=None, mono=False) if ctx is not None \
        else load_audio(file_path, sr=None, mono=False)
    return _target_profile_from_audio(y, int(sr))


def _flatten_library() -> list[tuple[str, int, list[float]]]:
    lib = _load_library()
    out: list[tuple[str, int, list[float]]] = []
    for genre, tracks in lib.get("genres", {}).items():
        for i, t in enumerate(tracks):
            out.append((genre, i, [float(t.get(k, 0.0)) for k in _METRIC_KEYS]))
    return out


def retrieve_nearest(measured: dict[str, float], genre_id: str | None = None,
                     k: int = 5) -> list[dict[str, Any]]:
    """k nearest reference exemplars with per-metric deltas. `genre_id` scopes
    the search to that genre (falls back to the whole library if it has no
    entries). Normalization uses whole-library spread for a consistent scale."""
    exemplars = _flatten_library()
    if not exemplars:
        return []
    arr = np.array([e[2] for e in exemplars])
    mean, std = arr.mean(axis=0), arr.std(axis=0) + 1e-9

    pool = [e for e in exemplars if e[0] == genre_id] if genre_id else exemplars
    if not pool:
        pool = exemplars
    mvec = np.array([measured[key] for key in _METRIC_KEYS])
    mz = (mvec - mean) / std

    scored = []
    for genre, idx, vec in pool:
        dist = float(np.linalg.norm(mz - (np.array(vec) - mean) / std))
        scored.append((dist, genre, idx, vec))
    scored.sort(key=lambda x: x[0])

    return [{
        "genre_id": genre,
        "reference_index": idx,
        "distance": round(dist, 3),
        "reference": {key: vec[j] for j, key in enumerate(_METRIC_KEYS)},
        "delta": {key: round(measured[key] - vec[j], 2) for j, key in enumerate(_METRIC_KEYS)},
    } for dist, genre, idx, vec in scored[:k]]


def _guidance(nearest: dict[str, Any]) -> str:
    d = nearest["delta"]
    bits = []
    if abs(d["lufs_integrated"]) >= 1.0:
        bits.append(f"{abs(d['lufs_integrated']):.1f} LU {'louder' if d['lufs_integrated'] > 0 else 'quieter'}")
    if abs(d["crest_factor_db"]) >= 1.0:
        bits.append(f"{abs(d['crest_factor_db']):.1f} dB {'less' if d['crest_factor_db'] < 0 else 'more'} dynamic")
    if abs(d["spectral_tilt_db_per_octave"]) >= 0.5:
        bits.append(f"{'brighter' if d['spectral_tilt_db_per_octave'] > 0 else 'darker'} tonal balance")
    if abs(d["lra"]) >= 1.0:
        bits.append(f"{abs(d['lra']):.1f} LU {'wider' if d['lra'] > 0 else 'narrower'} loudness range")
    where = f"nearest reference ({nearest['genre_id']})"
    return (f"Vs your {where}: " + ", ".join(bits) + "."
            if bits else f"Closely matches your {where} across all measured metrics.")


def _section_matches(y: np.ndarray, sr: int, genre_id: str | None,
                     k: int) -> list[dict[str, Any]]:
    """Retrieve references for detected sections with enough audio to measure.

    Sections come from the shared section detector so the time ranges agree
    with the rest of the analysis UI. A short section is omitted rather than
    presenting an unstable integrated-loudness comparison as authoritative.
    """
    from khensu_services.analysis.section_analyzer import (
        _MIN_SECTION_DURATION_SEC,
        detect_sections,
    )

    results: list[dict[str, Any]] = []
    for section in detect_sections(y, sr):
        start_sec = float(section["start_sec"])
        end_sec = float(section["end_sec"])
        duration_sec = end_sec - start_sec
        if duration_sec < _MIN_SECTION_DURATION_SEC:
            continue
        start_sample = max(0, int(start_sec * sr))
        end_sample = min(y.shape[-1], int(end_sec * sr))
        segment = y[..., start_sample:end_sample]
        if segment.shape[-1] < int(_MIN_SECTION_DURATION_SEC * sr):
            continue
        measured = _target_profile_from_audio(segment, sr)
        matches = retrieve_nearest(measured, genre_id, k)
        nearest = matches[0] if matches else None
        results.append({
            "label": str(section["label"]),
            "start_sec": round(start_sec, 3),
            "end_sec": round(end_sec, 3),
            "duration_sec": round(duration_sec, 3),
            "target_profile": measured,
            "matches": matches,
            "nearest": nearest,
            "guidance": _guidance(nearest) if nearest else "No reference library entries.",
        })
    return results


def analyze_reference_match(file_path: str, genre_id: str | None = None,
                            k: int = 5, ctx=None,
                            include_sections: bool = False) -> dict[str, Any]:
    """Target profile + nearest reference exemplars + actionable delta guidance."""
    if include_sections and ctx is None:
        y, sr = load_audio(file_path, sr=None, mono=False)
        sr = int(sr)
        measured = _target_profile_from_audio(y, sr)
        sections = _section_matches(y, sr, genre_id, k)
    else:
        measured = target_profile(file_path, ctx)
        sections = []
    matches = retrieve_nearest(measured, genre_id, k)
    lib = _load_library()
    nearest = matches[0] if matches else None
    return {
        "target_profile": measured,
        "genre_restriction": genre_id,
        "matches": matches,
        "nearest": nearest,
        "guidance": _guidance(nearest) if nearest else "No reference library entries.",
        "sections": sections,
        # Honest provenance: the bundled library is synthetic/bootstrap, so this
        # is directional until real analyzed references replace it (same caveat
        # as benchmark.py).
        "is_bootstrap_data": lib.get("source") == "bootstrap-synthetic",
    }
