# local-services/khensu_services/analysis/router_word_clip_gain.py
from __future__ import annotations
from typing import Any

from fastapi import APIRouter

from khensu_services.analysis.models import WordClipGainRequest, WordClipGainResponse
from khensu_services.common.errors import require_file
from khensu_services.common.output_paths import resolve_render_output
from khensu_services.common.executors import run_blocking

router = APIRouter()


@router.post("/render/word_clip_gain", response_model=WordClipGainResponse, response_model_exclude_unset=True)
async def render_word_clip_gain_endpoint(req: WordClipGainRequest) -> dict[str, Any]:
    require_file(req.file_path)
    out = resolve_render_output(req.output_path, req.file_path, "wordgain")

    from khensu_services.analysis.word_clip_gain import render_word_clip_gain
    regions = [r.model_dump() for r in req.regions]
    # Failures reach the global handler (S11): no per-route except/500.
    return await run_blocking(render_word_clip_gain, req.file_path, regions,
                              req.fade_ms, out)
