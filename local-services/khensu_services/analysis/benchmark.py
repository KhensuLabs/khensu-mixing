# local-services/khensu_services/analysis/benchmark.py
"""
Chart/reference benchmarking: percentile rank of this track's measured
values against a per-genre reference-measurement library, instead of the
distance-from-a-single-target-number scoring every other recommender in
this codebase uses.

Data-source note (flagged as an open question in the design spec): the
bundled library (reference_library/bootstrap_references.json) is bootstrap/
synthetic, sampled around each genre's hand-tuned production-norm targets —
not measurements of real reference tracks. Percentile output is directional
("more compressed than most of the synthetic distribution for this genre")
rather than a literal chart-competitive claim until real reference tracks
are analyzed and swapped in. The library schema (a flat list of
{lufs_integrated, lra, spectral_tilt_db_per_octave, crest_factor_db} per
genre id) is designed so real analyzed tracks can replace the synthetic
entries without any code change.
"""
from __future__ import annotations
import json
from pathlib import Path
from typing import Any

_LIBRARY_PATH = Path(__file__).parent / "reference_library" / "bootstrap_references.json"

_METRIC_KEYS = ("lufs_integrated", "lra", "spectral_tilt_db_per_octave", "crest_factor_db")

_library_cache: dict[str, Any] | None = None


def _load_library() -> dict[str, Any]:
    global _library_cache
    if _library_cache is None:
        with open(_LIBRARY_PATH, "r", encoding="utf-8") as f:
            _library_cache = json.load(f)
    return _library_cache


def _percentile_rank(value: float, sample: list[float]) -> float:
    """Percentage of the sample at or below value, 0..100. Standard
    percentile-of-sample-set — no statistical modeling beyond a sort+count."""
    if not sample:
        return 50.0
    below_or_equal = sum(1 for v in sample if v <= value)
    return round(100.0 * below_or_equal / len(sample), 1)


def benchmark_against_references(genre_id: str, lufs_integrated: float, lra: float,
                                  spectral_tilt_db_per_octave: float,
                                  crest_factor_db: float) -> dict[str, Any] | None:
    """Returns None if genre_id has no reference-library entry (the bundled
    bootstrap/synthetic library covers ~185 genres — see module docstring)."""
    library = _load_library()
    genre_tracks = library.get("genres", {}).get(genre_id)
    if not genre_tracks:
        return None

    measured = {
        "lufs_integrated": lufs_integrated,
        "lra": lra,
        "spectral_tilt_db_per_octave": spectral_tilt_db_per_octave,
        "crest_factor_db": crest_factor_db,
    }
    percentile_by_metric = {
        key: _percentile_rank(measured[key], [t[key] for t in genre_tracks])
        for key in _METRIC_KEYS
    }

    summary_bits = []
    if percentile_by_metric["crest_factor_db"] <= 20.0:
        summary_bits.append("more compressed (lower crest factor) than most references")
    elif percentile_by_metric["crest_factor_db"] >= 80.0:
        summary_bits.append("more dynamic (higher crest factor) than most references")
    if percentile_by_metric["lufs_integrated"] >= 80.0:
        summary_bits.append("louder than most references")
    elif percentile_by_metric["lufs_integrated"] <= 20.0:
        summary_bits.append("quieter than most references")
    if percentile_by_metric["spectral_tilt_db_per_octave"] >= 80.0:
        summary_bits.append("brighter tonal balance than most references")
    elif percentile_by_metric["spectral_tilt_db_per_octave"] <= 20.0:
        summary_bits.append("darker tonal balance than most references")

    competitive_summary = ("; ".join(summary_bits).capitalize() + " for " + genre_id + "."
                            if summary_bits else
                            f"Sits within the typical range of references for {genre_id} across all measured metrics.")

    return {
        "genre_id": genre_id,
        "percentile_by_metric": percentile_by_metric,
        "competitive_summary": competitive_summary,
        "sample_size": len(genre_tracks),
        "is_bootstrap_data": library.get("source") == "bootstrap-synthetic",
    }
