from __future__ import annotations

import os

from fastapi import APIRouter

from khensu_services.common.api_models import RequestModel

from khensu_services.analysis.analyzer_responses import DitherPreviewResponse
from khensu_services.analysis.dither_preview import analyze_dither
from khensu_services.common.errors import invalid_input, require_file
from khensu_services.common.executors import run_blocking

router = APIRouter()


class DitherRequest(RequestModel):
    file_path: str
    target_bits: int = 16
    output_dir: str = ""


@router.post("/analyze/dither/preview", response_model=DitherPreviewResponse, response_model_exclude_unset=True)
async def dither_preview(req: DitherRequest) -> dict:
    """Compare truncation vs TPDF vs shaped dither at the target bit depth and
    render the dithered result for audition (noise-floor delta + audition)."""
    require_file(req.file_path)
    if req.target_bits not in (8, 16, 24):
        invalid_input("target_bits must be 8, 16, or 24")
    if req.output_dir:
        from khensu_services.common.output_paths import validate_output_path
        validate_output_path(os.path.join(req.output_dir, "probe.wav"), req.file_path)
    return await run_blocking(analyze_dither, req.file_path, req.target_bits,
                              req.output_dir or None)
