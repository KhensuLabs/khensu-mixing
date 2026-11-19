from __future__ import annotations

import os

from fastapi import APIRouter
from pydantic import Field

from khensu_services.common.api_models import RequestModel

from khensu_services.analysis.analyzer_responses import CodecAuditResponse, CodecPlatformsResponse
from khensu_services.analysis.codec_artifacts import audit_codecs, PLATFORM_CODECS
from khensu_services.common.errors import require_file
from khensu_services.common.executors import run_blocking
from khensu_services.common.lru import BoundedCache, file_cache_key

router = APIRouter()

# Encode/decode through every codec is the slowest analysis on the dashboard.
# Cached per (source stamp, codecs, output_dir); a hit is only served while
# every decoded A/B file it points at still exists.
_codec_cache: BoundedCache[tuple, dict] = BoundedCache(max_entries=16)


def _decoded_files_exist(result: dict) -> bool:
    return all(os.path.isfile(r["decoded_path"])
               for r in result.get("results", []) if r.get("decoded_path"))


class CodecAuditRequest(RequestModel):
    file_path: str
    codecs: list[str] = Field(default_factory=list)   # empty = default set
    output_dir: str = ""


@router.get("/analyze/codec/platforms", response_model=CodecPlatformsResponse, response_model_exclude_unset=True)
async def codec_platforms() -> dict:
    return {"platforms": [{"id": k, **v} for k, v in PLATFORM_CODECS.items()]}


@router.post("/analyze/codec/audit", response_model=CodecAuditResponse, response_model_exclude_unset=True)
async def codec_audit(req: CodecAuditRequest) -> dict:
    """Encode the master through each platform codec and measure the artifacts
    added (HF/air loss, stereo-width collapse, true-peak rise, pre-echo). The
    decoded WAVs are written for A/B auditioning. ffmpeg-gated; reports
    encoder_available=False when ffmpeg is absent."""
    require_file(req.file_path)
    if req.output_dir:
        from khensu_services.common.output_paths import validate_output_path
        validate_output_path(os.path.join(req.output_dir, "probe.wav"), req.file_path)
    ck = file_cache_key(req.file_path, "codec_audit", sorted(req.codecs), req.output_dir)
    cached = _codec_cache.get(ck) if ck is not None else None
    if cached is not None and _decoded_files_exist(cached):
        return cached
    data = await run_blocking(audit_codecs, req.file_path, req.codecs or None,
                              req.output_dir or None)
    # ffmpeg missing is not cached: installing it must take effect at once.
    if ck is not None and data.get("encoder_available"):
        _codec_cache[ck] = data
    return data
