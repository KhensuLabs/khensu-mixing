"""POST /delivery/stream_preview - the master as a streaming listener hears
it: a real ffmpeg codec round trip (or the labelled simulation), the
platform's normalisation and limiter, and a playback-device profile, with
loudness-matched A/B files (CE-DEL-3)."""
from __future__ import annotations

import logging
import os
from typing import Optional

from fastapi import APIRouter

from khensu_services.common.api_models import RequestModel, ResponseModel
from khensu_services.common.errors import (
    conflict, internal_error, invalid_input, require_decode_budget, require_file)
from khensu_services.common.executors import run_long_blocking

_log = logging.getLogger(__name__)
router = APIRouter()


class StreamPreviewRequest(RequestModel):
    file_path: str
    platform: str = "spotify"         # a streaming delivery-target id (spotify_loud too)
    codec: str = ""                   # aac | opus | mp3; empty = the platform's default
    bitrate_kbps: int = 0             # 0 = the platform's default
    device: str = ""                  # phone | earbuds | car | club; empty = none
    output_dir: str = ""              # empty = <source dir>/stream_preview


class StreamStageLevels(ResponseModel):
    integrated_lufs: Optional[float] = None
    true_peak_dbtp: Optional[float] = None


class StreamCodecArtifacts(ResponseModel):
    hf_loss_db: float
    air_loss_db: float
    stereo_width_delta: float
    true_peak_delta_dbtp: float
    pre_echo_score: float


class StreamPreviewResponse(ResponseModel):
    file_path: str
    platform: str
    platform_name: str
    codec: str
    bitrate_kbps: int
    device: Optional[str] = None
    codec_mode: str                   # ffmpeg | simulated
    simulated_reason: Optional[str] = None
    sample_rate: int
    alignment_lag_samples: int
    master: StreamStageLevels
    decoded: StreamStageLevels        # after the codec round trip
    after_platform: StreamStageLevels
    final: StreamStageLevels          # after the device profile
    reference: StreamStageLevels      # no-codec A side
    codec_distortion_db: Optional[float] = None
    true_peak_delta_db: Optional[float] = None
    artifacts: StreamCodecArtifacts
    platform_gain_db: Optional[float] = None
    limiter_engaged: bool
    match_gain_db: Optional[float] = None
    preview_path: str
    matched_preview_path: str
    reference_path: str
    encoded_path: Optional[str] = None
    note: Optional[str] = None
    notes: list[str]


class StreamPreviewOptionsResponse(ResponseModel):
    platforms: list[str]
    codecs: list[str]
    devices: list[str]
    ffmpeg_available: bool


@router.get("/delivery/stream_preview/options", response_model=StreamPreviewOptionsResponse,
            response_model_exclude_unset=True)
async def stream_preview_options() -> dict:
    from khensu_services.analysis.codec_artifacts import STREAM_CODECS
    from khensu_services.analysis.stream_preview import ffmpeg_path
    from khensu_services.analysis.streaming_normalization import PLATFORMS
    from khensu_services.copilot.device_sim import list_device_profiles
    return {"platforms": [p["id"] for p in PLATFORMS], "codecs": list(STREAM_CODECS),
            "devices": [d["device"] for d in list_device_profiles()],
            "ffmpeg_available": ffmpeg_path() is not None}


@router.post("/delivery/stream_preview", response_model=StreamPreviewResponse,
             response_model_exclude_unset=True)
async def stream_preview_endpoint(req: StreamPreviewRequest) -> dict:
    from khensu_services.analysis.stream_preview import UnsupportedLayoutError, stream_preview
    from khensu_services.common.output_paths import reject_output_alias, validate_output_path

    require_file(req.file_path)
    out_dir = req.output_dir.strip() or os.path.join(
        os.path.dirname(os.path.abspath(req.file_path)), "stream_preview")
    probe = os.path.join(out_dir, "probe.wav")
    validate_output_path(probe, req.file_path)
    reject_output_alias(probe, req.file_path)
    await require_decode_budget(req.file_path)
    try:
        # ffmpeg encode + decode + several full-signal passes: long-job pool.
        return await run_long_blocking(stream_preview, req.file_path, req.platform, req.codec,
                                       req.bitrate_kbps, req.device, out_dir)
    except UnsupportedLayoutError as exc:
        conflict(str(exc))   # a deliberate layout refusal, like a render guardrail
    except ValueError as exc:
        invalid_input(str(exc))
    except Exception:
        _log.exception("Stream preview failed for %s", req.file_path)
        internal_error()
