# local-services/khensu_services/analysis/router_counterfactual.py
"""CE-TRU-2: ``/explain/counterfactual`` - the smallest parameter change that
clears a finding, verified by rendering through the DSP chain and
re-measuring."""
from __future__ import annotations

import time
from typing import Any, Optional

from fastapi import APIRouter
from pydantic import Field

from khensu_services.common.api_models import RequestModel, ResponseModel
from khensu_services.common.errors import invalid_input, require_decode_budget, require_file
from khensu_services.common.executors import run_long_blocking

router = APIRouter()


class CounterfactualRequest(RequestModel):
    finding_code: str = Field(..., min_length=1, max_length=120,
                              description="Issue code, e.g. lufs_too_loud, tilt_too_bright, harshness_detected_3k.")
    file_path: Optional[str] = Field(None, description="Audio to search on; without it only loudness / "
                                                       "true-peak findings are answered (analytically).")
    measured_value: Optional[float] = None
    threshold: Optional[float] = None
    freq_hz: Optional[float] = Field(None, ge=20.0, le=20000.0)
    genre_id: Optional[str] = None
    preview: bool = Field(True, description="Render the winning change and re-measure the file.")
    output_path: Optional[str] = Field(None, description="Preview WAV path (default: beside the source).")


class CounterfactualEffect(ResponseModel):
    effect: str
    parameters: dict[str, float]


class CounterfactualStep(ResponseModel):
    parameter: float
    value: float


class CounterfactualSideEffects(ResponseModel):
    lufs_before: Optional[float] = None
    lufs_after: Optional[float] = None
    true_peak_before: Optional[float] = None
    true_peak_after: Optional[float] = None


class CounterfactualResponse(ResponseModel):
    finding_code: str
    family: str
    metric: str
    metric_unit: str
    direction: str
    parameter_name: str
    parameter_unit: str
    freq_hz: Optional[float] = None
    measured_value_supplied: Optional[float] = None
    threshold: float
    threshold_source: str
    measured_before: float
    status: str
    parameter_value: float
    change_text: str
    effects: list[CounterfactualEffect]
    after_value: float
    cleared: bool
    verified: bool
    method: str
    iterations: int
    trace: list[CounterfactualStep]
    preview_path: Optional[str] = None
    side_effects: Optional[CounterfactualSideEffects] = None
    explanation: str


def _run(req: CounterfactualRequest, preview_path: Optional[str]) -> dict[str, Any]:
    from khensu_services.analysis.counterfactual import UnsupportedFinding, counterfactual
    try:
        return counterfactual(finding_code=req.finding_code, file_path=req.file_path or None,
                              threshold=req.threshold, measured_value=req.measured_value,
                              freq_hz=req.freq_hz, genre_id=req.genre_id or None,
                              preview_path=preview_path)
    except UnsupportedFinding as exc:
        invalid_input(str(exc))
    except ValueError as exc:
        msg = str(exc)
        if msg.startswith(("threshold:", "freq_hz:", "file_path:")):
            invalid_input(msg)
        raise


@router.post("/explain/counterfactual", response_model=CounterfactualResponse,
             response_model_exclude_unset=True)
async def explain_counterfactual(req: CounterfactualRequest) -> dict[str, Any]:
    from khensu_services.analysis.counterfactual import UnsupportedFinding, classify
    try:
        classify(req.finding_code)
    except UnsupportedFinding as exc:
        invalid_input(str(exc))
    preview_path = None
    if req.file_path:
        require_file(req.file_path)
        await require_decode_budget(req.file_path)
        if req.preview:
            from khensu_services.common.output_paths import resolve_render_output
            safe_code = "".join(ch if ch.isalnum() else "_" for ch in req.finding_code)[:40]
            preview_path = resolve_render_output(
                req.output_path, req.file_path, f"cf_{safe_code}_{int(time.time() * 1000)}",
                allowed_exts=(".wav",))
    elif req.output_path:
        invalid_input("output_path needs file_path")
    return await run_long_blocking(_run, req, preview_path)
