# local-services/khensu_services/analysis/router_watermark_models.py
"""HTTP surface for pre-caching the learned-watermark model weights.

Lets the desktop (or a first-run script) download the AudioSeal + WavMark
weights ahead of time so enabling ``KHENSU_ENABLE_WATERMARK_MODELS`` never
blocks the analysis thread on an HF-Hub download. See
``khensu_services.analysis.artifacts.watermark_precache``.
"""
from __future__ import annotations

from fastapi import APIRouter
from khensu_services.common.errors import online_disabled

from khensu_services.analysis.analyzer_responses import (
    WatermarkPrecacheResponse, WatermarkStatusResponse,
)
from khensu_services.analysis.artifacts import watermark_precache as wp
from khensu_services.common.executors import io_route

router = APIRouter()


@router.get("/analyze/models/watermark/status", response_model=WatermarkStatusResponse, response_model_exclude_unset=True)
@io_route
def watermark_models_status() -> dict:
    """Package availability, per-model cached flag, and any running job's state."""
    return wp.status()


@router.post("/analyze/models/watermark/precache", response_model=WatermarkPrecacheResponse, response_model_exclude_unset=True)
@io_route
def watermark_models_precache() -> dict:
    """Start (or no-op if already running) a background pre-cache of the
    watermark weights. Poll ``/models/watermark/status`` for completion."""
    # Both toggles gate an HF Hub fetch (model_downloads AND hf_hub).
    if not wp.hf_downloads_allowed():
        from khensu_services.common.security_log import KIND_ONLINE_GATE_BLOCKED, record_event
        record_event(KIND_ONLINE_GATE_BLOCKED, "watermark model precache")
        online_disabled("Model downloads are disabled in Settings")
    precacher = wp.WatermarkPrecacher()
    precacher.start()
    return {"started": True, **precacher.status_dict()}
