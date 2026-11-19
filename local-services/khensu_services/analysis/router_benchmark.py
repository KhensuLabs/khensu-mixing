from __future__ import annotations
from fastapi import APIRouter

from typing import Any

from khensu_services.analysis.benchmark import _load_library
from khensu_services.common.api_models import ResponseModel
from khensu_services.common.errors import not_found

router = APIRouter()


class ReferencePointsResponse(ResponseModel):
    genre_id: str
    is_bootstrap_data: bool
    # Per-track metric samples from the reference library (one dict per track).
    points: list[dict[str, Any]]


@router.get("/benchmark/reference_points", response_model=ReferencePointsResponse,
            response_model_exclude_unset=True)
async def benchmark_reference_points(genre: str) -> dict:
    """Raw per-genre sample points backing the percentile-rank summary
    `/analyze/extended` already returns — used by the desktop app's
    Reference Library 3D comparative view, which needs the actual point
    cloud, not just a rolled-up percentile. Same bootstrap-synthetic
    library as benchmark.py (see its module docstring)."""
    library = _load_library()
    genre_tracks = library.get("genres", {}).get(genre)
    if not genre_tracks:
        not_found(f"No reference data for genre: {genre}")
    return {
        "genre_id": genre,
        "is_bootstrap_data": library.get("source") == "bootstrap-synthetic",
        "points": genre_tracks,
    }
