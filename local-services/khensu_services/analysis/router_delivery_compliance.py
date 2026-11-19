# local-services/khensu_services/analysis/router_delivery_compliance.py
"""POST /analyze/delivery_compliance - grade a file against broadcast /
podcast (or any) delivery loudness targets from ``delivery_targets``."""
from __future__ import annotations

import logging
from typing import Optional

from fastapi import APIRouter
from pydantic import Field

from khensu_services.analysis.delivery_targets import compliance_for_file, target_by_id
from khensu_services.common.api_models import RequestModel, ResponseModel
from khensu_services.common.errors import (
    internal_error, invalid_input, require_decode_budget, require_file)
from khensu_services.common.executors import run_blocking

_log = logging.getLogger(__name__)
router = APIRouter()


class DeliveryComplianceRequest(RequestModel):
    file_path: str
    # Target ids from GET /analyze/export-targets; empty = every broadcast and
    # podcast target.
    target_ids: list[str] = Field(default_factory=list, max_length=32)


class DeliveryMeasurements(ResponseModel):
    integrated_lufs: Optional[float] = None
    true_peak_dbtp: Optional[float] = None
    lra_lu: Optional[float] = None
    short_term_max_lufs: Optional[float] = None


class DeliveryCheck(ResponseModel):
    id: str
    status: str                       # pass | warn | fail
    measured: Optional[float] = None
    target: Optional[float] = None
    delta: Optional[float] = None     # measured - target
    note: str
    fix: str


class DeliveryTargetResult(ResponseModel):
    target_id: str
    name: str
    category: str
    measurement: str                  # as_delivered | mono
    overall: str                      # pass | warn | fail
    checks: list[DeliveryCheck]


class DeliveryComplianceResponse(ResponseModel):
    file_path: str
    channels: int
    measurements: DeliveryMeasurements
    mono_measurements: DeliveryMeasurements
    results: list[DeliveryTargetResult]


@router.post("/analyze/delivery_compliance", response_model=DeliveryComplianceResponse,
             response_model_exclude_unset=True)
async def delivery_compliance(req: DeliveryComplianceRequest) -> dict:
    require_file(req.file_path)
    unknown = [t for t in req.target_ids if target_by_id(t) is None]
    if unknown:
        invalid_input(f"Unknown delivery target(s): {', '.join(unknown)}")
    await require_decode_budget(req.file_path)
    try:
        return await run_blocking(compliance_for_file, req.file_path, req.target_ids or None)
    except ValueError as exc:            # non-finite source / unsupported channel layout
        invalid_input(str(exc))
    except Exception:
        # A file that passed the header probe but fails to decode is still
        # answered 422 by the server's handler (it walks __context__).
        _log.exception("Delivery compliance failed for %s", req.file_path)
        internal_error()
