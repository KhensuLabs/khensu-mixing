"""POST /delivery/immersive_check - multichannel / ADM BWF delivery check
(CE-DEL-1): layout, BS.1770-4 multichannel loudness, per-channel true peak,
stereo fold-down and binaural previews, graded against the Atmos-music
profile in ``delivery_targets.IMMERSIVE_TARGETS``."""
from __future__ import annotations

import logging
import os
from typing import Optional

from fastapi import APIRouter
from pydantic import Field

from khensu_services.analysis.router_delivery_compliance import DeliveryCheck
from khensu_services.common.api_models import RequestModel, ResponseModel
from khensu_services.common.errors import internal_error, invalid_input, require_file, too_large
from khensu_services.common.executors import run_io_blocking, run_long_blocking

_log = logging.getLogger(__name__)
router = APIRouter()


class ImmersiveCheckRequest(RequestModel):
    file_path: str
    target_id: str = "atmos_music"
    # Where the fold-down / binaural previews go; empty = <source dir>/<name>_immersive.
    output_dir: str = ""
    render_previews: bool = True


class ImmersiveAdm(ResponseModel):
    present: bool
    chna_tracks: int
    programmes: list[str]
    packs: list[str]
    has_objects: bool
    object_count: int
    parse_error: Optional[str] = None


class ImmersiveChannel(ResponseModel):
    index: int
    label: str
    azimuth: float
    elevation: float
    is_lfe: bool
    kind: str                       # speaker | object
    weight: float                   # BS.1770-4 channel weight (0 = LFE)
    true_peak_dbtp: Optional[float] = None
    rms_dbfs: Optional[float] = None
    silent: bool


class ImmersiveFoldDown(ResponseModel):
    integrated_lufs: Optional[float] = None
    loudness_delta_lu: Optional[float] = None     # fold-down minus immersive programme
    true_peak_dbtp: Optional[float] = None
    lr_correlation: Optional[float] = None
    cancellation_left_db: Optional[float] = None  # < 0: content cancels when summed
    cancellation_right_db: Optional[float] = None
    issues: list[str]
    output_path: Optional[str] = None


class ImmersiveBinaural(ResponseModel):
    integrated_lufs: Optional[float] = None
    loudness_delta_lu: Optional[float] = None
    true_peak_dbtp: Optional[float] = None
    interaural_correlation: Optional[float] = None
    model: str
    output_path: Optional[str] = None


class ImmersiveCompliance(ResponseModel):
    target_id: str
    name: str
    category: str
    measurement: str
    overall: str                    # pass | warn | fail
    checks: list[DeliveryCheck]


class ImmersiveCheckResponse(ResponseModel):
    file_path: str
    container: str
    sample_rate: int
    bit_depth: Optional[int] = None
    channels: int
    frames: int
    duration_sec: float
    layout: str                     # e.g. 7.1.4, 5.1, stereo
    layout_source: str              # adm | channel_mask | channel_count
    channel_mask: Optional[int] = None
    adm: ImmersiveAdm
    channel_list: list[ImmersiveChannel]
    integrated_lufs: Optional[float] = None
    max_true_peak_dbtp: Optional[float] = None
    fold_down: ImmersiveFoldDown
    binaural: ImmersiveBinaural
    compliance: ImmersiveCompliance
    notes: list[str]


class ImmersiveTarget(ResponseModel):
    id: str
    name: str
    category: str
    lufs_integrated_max: float
    true_peak_dbtp: float
    sample_rate: int
    bit_depth: int
    standard: str
    notes: str


class ImmersiveTargetsResponse(ResponseModel):
    targets: list[ImmersiveTarget] = Field(default_factory=list)


@router.get("/delivery/immersive_targets", response_model=ImmersiveTargetsResponse,
            response_model_exclude_unset=True)
async def immersive_targets() -> dict:
    from khensu_services.analysis.delivery_targets import IMMERSIVE_TARGETS
    keys = ImmersiveTarget.model_fields.keys()
    return {"targets": [{k: t[k] for k in keys} for t in IMMERSIVE_TARGETS]}


def _budget(path: str) -> None:
    """Header probe + the immersive working-set budget (the service's generic
    decode budget caps channels at 8, which a 7.1.4 bed exceeds)."""
    from khensu_services.analysis.immersive import ChannelSource, check_budget
    check_budget(ChannelSource(path))


@router.post("/delivery/immersive_check", response_model=ImmersiveCheckResponse,
             response_model_exclude_unset=True)
async def immersive_check_endpoint(req: ImmersiveCheckRequest) -> dict:
    from khensu_services.analysis.delivery_targets import immersive_target_by_id
    from khensu_services.analysis.immersive import (
        ImmersiveBudgetError, ImmersiveFormatError, immersive_check)

    require_file(req.file_path)
    if immersive_target_by_id(req.target_id) is None:
        invalid_input(f"Unknown immersive target: {req.target_id}")
    out_dir = req.output_dir.strip() or None
    if req.render_previews:
        from khensu_services.common.output_paths import reject_output_alias, validate_output_path
        probe_dir = out_dir or os.path.join(
            os.path.dirname(os.path.abspath(req.file_path)),
            os.path.splitext(os.path.basename(req.file_path))[0] + "_immersive")
        probe_path = os.path.join(probe_dir, "probe.wav")
        validate_output_path(probe_path, req.file_path)
        reject_output_alias(probe_path, req.file_path)
    try:
        await run_io_blocking(_budget, req.file_path)
    except ImmersiveBudgetError as exc:
        too_large(str(exc), cause=exc)
    except (ImmersiveFormatError, OSError) as exc:
        invalid_input(f"Invalid audio file: {exc}")
    try:
        # Several full passes over a multichannel file: the long-job pool,
        # never the interactive DSP pool.
        return await run_long_blocking(immersive_check, req.file_path, req.target_id, out_dir,
                                       req.render_previews)
    except ImmersiveBudgetError as exc:
        too_large(str(exc), cause=exc)
    except (ImmersiveFormatError, ValueError) as exc:
        invalid_input(str(exc))
    except Exception:
        _log.exception("Immersive check failed for %s", req.file_path)
        internal_error()
