# local-services/khensu_services/analysis/router_embeddings.py
"""CE-AI-1: audio embeddings, the local reference index, similar-sound,
text-to-audio (CLAP only) and in-track section search."""
from __future__ import annotations

from typing import Any, Literal, Optional

from fastapi import APIRouter
from pydantic import Field

from khensu_services.common.api_models import RequestModel, ResponseModel
from khensu_services.common.errors import (
    invalid_input,
    not_found,
    require_decode_budget,
    require_file,
    unavailable,
)
from khensu_services.common.executors import run_blocking, run_long_blocking

router = APIRouter()

_MAX_INDEX_FILES = 200
Backend = Literal["auto", "clap", "handcrafted"]


def _resolve(requested: str) -> dict[str, Any]:
    from khensu_services.analysis.embeddings import EmbeddingBackendUnavailable, backend_status
    try:
        return backend_status(requested)
    except EmbeddingBackendUnavailable as exc:
        unavailable(str(exc), cause=exc)


async def _run(fn, *args, long: bool = False, **kwargs):
    """Run an embeddings call off the loop; map a backend failure to 503 and a
    bad region to 422."""
    from khensu_services.analysis.embeddings import EmbeddingBackendUnavailable
    try:
        if long:
            return await run_long_blocking(fn, *args, **kwargs)
        return await run_blocking(fn, *args, **kwargs)
    except EmbeddingBackendUnavailable as exc:
        unavailable(str(exc), cause=exc)
    except ValueError as exc:
        if "region" in str(exc):
            invalid_input(str(exc))
        raise


class EmbeddingBackendInfo(ResponseModel):
    backend: str
    reason: str
    clap_installed: bool
    clap_weights_cached: bool
    downloads_allowed: bool


class EmbeddingIndexEntry(ResponseModel):
    id: str
    path: str
    label: str
    duration_s: float
    added_at: float
    exists: bool
    tags: list[str] = []
    genre: str = ""


class EmbeddingIndexResult(ResponseModel):
    path: str
    id: str
    status: str
    reason: Optional[str] = None


class EmbeddingIndexRequest(RequestModel):
    file_paths: list[str] = Field(..., min_length=1)
    labels: Optional[list[str]] = None
    backend: Backend = "auto"


class EmbeddingIndexResponse(ResponseModel):
    backend: EmbeddingBackendInfo
    results: list[EmbeddingIndexResult]
    index_size: int


class EmbeddingIndexListResponse(ResponseModel):
    backend: EmbeddingBackendInfo
    entries: list[EmbeddingIndexEntry]
    index_size: int


class EmbeddingMetaRequest(RequestModel):
    """The user's library metadata for one entry (FEAT-5); an omitted field
    is left as it is."""
    id: str = Field(..., min_length=1, max_length=64)
    tags: Optional[list[str]] = Field(None, max_length=16)
    genre: Optional[str] = Field(None, max_length=80)


class EmbeddingRemoveRequest(RequestModel):
    ids: list[str] = Field(..., min_length=1)
    backend: Backend = "auto"


class EmbeddingRemoveResponse(ResponseModel):
    removed: int
    index_size: int


class EmbeddingSimilarRequest(RequestModel):
    file_path: Optional[str] = None
    entry_id: Optional[str] = None
    start_s: Optional[float] = Field(None, ge=0.0)
    end_s: Optional[float] = Field(None, ge=0.0)
    k: int = Field(10, ge=1, le=50)
    backend: Backend = "auto"
    #: Only entries with this tag / genre (FEAT-5).
    tag: Optional[str] = Field(None, max_length=40)
    genre: Optional[str] = Field(None, max_length=80)


class EmbeddingQuery(ResponseModel):
    id: str
    path: str


class EmbeddingMatch(ResponseModel):
    id: str
    path: str
    label: str
    duration_s: float
    added_at: float
    exists: bool
    tags: list[str] = []
    genre: str = ""
    similarity: float


class EmbeddingSimilarResponse(ResponseModel):
    backend: EmbeddingBackendInfo
    query: EmbeddingQuery
    results: list[EmbeddingMatch]
    index_size: int


class EmbeddingTextSearchRequest(RequestModel):
    text: str = Field(..., min_length=1, max_length=500)
    k: int = Field(10, ge=1, le=50)
    tag: Optional[str] = Field(None, max_length=40)
    genre: Optional[str] = Field(None, max_length=80)


class EmbeddingTextSearchResponse(ResponseModel):
    backend: EmbeddingBackendInfo
    text: str
    results: list[EmbeddingMatch]
    index_size: int


class EmbeddingSectionRequest(RequestModel):
    file_path: str
    start_s: float = Field(..., ge=0.0)
    end_s: float = Field(..., gt=0.0)
    k: int = Field(5, ge=1, le=50)
    window_s: Optional[float] = Field(None, ge=1.0, le=30.0)
    backend: Backend = "auto"


class EmbeddingSectionMatch(ResponseModel):
    start_s: float
    end_s: float
    similarity: float


class EmbeddingSectionResponse(ResponseModel):
    backend: EmbeddingBackendInfo
    query_start_s: float
    query_end_s: float
    window_s: float
    duration_s: float
    matches: list[EmbeddingSectionMatch]
    windows_compared: int


@router.post("/embeddings/index", response_model=EmbeddingIndexResponse,
             response_model_exclude_unset=True)
async def embeddings_index(req: EmbeddingIndexRequest) -> dict[str, Any]:
    if len(req.file_paths) > _MAX_INDEX_FILES:
        invalid_input(f"At most {_MAX_INDEX_FILES} files per index request")
    if req.labels is not None and len(req.labels) != len(req.file_paths):
        invalid_input("labels must have one entry per file path")
    from khensu_services.common.local_paths import require_local_path
    for p in req.file_paths:
        require_local_path(p, "Reference")
        # A missing file is the request's 404 (common/errors.py), the same as
        # an undecodable one is its 422 - not a per-file "failed" row.
        require_file(p, "Reference")
    await require_decode_budget(*req.file_paths)
    info = _resolve(req.backend)
    from khensu_services.analysis.embeddings import index_files
    data = await _run(index_files, req.file_paths, req.labels, info["backend"], long=True)
    return {"backend": info, **data}


@router.get("/embeddings/index", response_model=EmbeddingIndexListResponse,
            response_model_exclude_unset=True)
async def embeddings_index_list(backend: Backend = "auto", tag: Optional[str] = None,
                                genre: Optional[str] = None) -> dict[str, Any]:
    info = _resolve(backend)
    from khensu_services.analysis.embeddings import list_index
    entries = await _run(list_index, info["backend"], tag, genre)
    return {"backend": info, "entries": entries, "index_size": len(entries)}


@router.post("/embeddings/index/meta", response_model=EmbeddingIndexEntry,
             response_model_exclude_unset=True)
async def embeddings_index_meta(req: EmbeddingMetaRequest) -> dict[str, Any]:
    """Tag a library entry and/or give it a genre (FEAT-5)."""
    from khensu_services.analysis.embeddings import set_entry_meta
    try:
        return await _run(set_entry_meta, req.id, req.tags, req.genre)
    except KeyError:
        not_found(f"Index entry not found: {req.id}")


@router.post("/embeddings/index/remove", response_model=EmbeddingRemoveResponse,
             response_model_exclude_unset=True)
async def embeddings_index_remove(req: EmbeddingRemoveRequest) -> dict[str, Any]:
    info = _resolve(req.backend)
    from khensu_services.analysis.embeddings import remove_entries
    return await _run(remove_entries, req.ids, info["backend"])


@router.post("/embeddings/similar", response_model=EmbeddingSimilarResponse,
             response_model_exclude_unset=True)
async def embeddings_similar(req: EmbeddingSimilarRequest) -> dict[str, Any]:
    if bool(req.file_path) == bool(req.entry_id):
        invalid_input("Give exactly one of file_path or entry_id")
    if req.file_path:
        require_file(req.file_path)
        await require_decode_budget(req.file_path)
    if req.start_s is not None and req.end_s is not None and req.end_s <= req.start_s:
        invalid_input("end_s must be greater than start_s")
    info = _resolve(req.backend)
    from khensu_services.analysis.embeddings import similar
    try:
        data = await _run(similar, info["backend"], file_path=req.file_path, entry=req.entry_id,
                          start_s=req.start_s, end_s=req.end_s, k=req.k, tag=req.tag, genre=req.genre)
    except KeyError:
        not_found(f"Index entry not found: {req.entry_id}")
    return {"backend": info, **data}


@router.post("/embeddings/search_text", response_model=EmbeddingTextSearchResponse,
             response_model_exclude_unset=True)
async def embeddings_search_text(req: EmbeddingTextSearchRequest) -> dict[str, Any]:
    info = _resolve("auto")
    if info["backend"] != "clap":
        unavailable(f"Text search needs the CLAP backend: {info['reason']}")
    from khensu_services.analysis.embeddings import search_text
    data = await _run(search_text, req.text.strip(), req.k, req.tag, req.genre)
    return {"backend": info, "text": req.text.strip(), **data}


@router.post("/embeddings/sections", response_model=EmbeddingSectionResponse,
             response_model_exclude_unset=True)
async def embeddings_sections(req: EmbeddingSectionRequest) -> dict[str, Any]:
    require_file(req.file_path)
    await require_decode_budget(req.file_path)
    if req.end_s <= req.start_s:
        invalid_input("end_s must be greater than start_s")
    info = _resolve(req.backend)
    from khensu_services.analysis.embeddings import section_search
    data = await _run(section_search, req.file_path, req.start_s, req.end_s, info["backend"],
                      k=req.k, window_s=req.window_s)
    return {"backend": info, "query_start_s": req.start_s, "query_end_s": req.end_s, **data}
