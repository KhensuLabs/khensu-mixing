# local-services/khensu_services/analysis/router_genre_examples.py
"""GEN-1e: the user's confirmed or chosen genres, which the genre probe
blends in (``analysis/learned/genre_examples.py``)."""
from __future__ import annotations

from typing import Any, Literal

from fastapi import APIRouter

from khensu_services.common.api_models import OpenResponseModel, RequestModel, ResponseModel
from khensu_services.common.errors import invalid_input, require_decode_budget, require_file
from khensu_services.common.executors import run_blocking, run_io_blocking

router = APIRouter()


class GenreExampleRequest(RequestModel):
    file_path: str
    genre: str
    source: Literal["confirmed", "chosen"] = "confirmed"


class GenreExampleResponse(ResponseModel):
    stored: bool
    reason: str | None = None
    examples: int


class GenreExampleCounts(OpenResponseModel):
    """Examples per catalog genre id."""


class GenreExamplesSummaryResponse(ResponseModel):
    examples: int
    genres: GenreExampleCounts


class GenreExamplesClearResponse(ResponseModel):
    removed: int


@router.post("/genre/examples", response_model=GenreExampleResponse, response_model_exclude_unset=True)
async def add_genre_example(req: GenreExampleRequest) -> dict[str, Any]:
    """Remember the genre the user settled on for this track. Stores the
    genre probe's features, not the audio or its path; without the probe
    nothing is stored (``stored: false``)."""
    from khensu_services.analysis.learned import genre_examples

    require_file(req.file_path)
    await require_decode_budget(req.file_path)
    try:
        return await run_blocking(genre_examples.add, req.file_path, req.genre, req.source)
    except ValueError as exc:
        invalid_input(str(exc), cause=exc)


@router.get("/genre/examples", response_model=GenreExamplesSummaryResponse, response_model_exclude_unset=True)
async def genre_examples_summary() -> dict[str, Any]:
    """How many genre examples are stored, per genre."""
    from khensu_services.analysis.learned import genre_examples

    return await run_io_blocking(genre_examples.summary)


@router.delete("/genre/examples", response_model=GenreExamplesClearResponse, response_model_exclude_unset=True)
async def clear_genre_examples() -> dict[str, Any]:
    """Forget every stored genre example."""
    from khensu_services.analysis.learned import genre_examples

    return {"removed": await run_io_blocking(genre_examples.clear)}
