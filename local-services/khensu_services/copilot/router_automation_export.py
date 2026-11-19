# local-services/khensu_services/copilot/router_automation_export.py
"""/automation/export (CE-DAW-4): a fix or explicit parameter curves ->
a ``.mid`` with one CC lane per Khensu Copilot parameter plus a REAPER
FX-envelope chunk file, so a fix can be applied in the DAW without a render.
"""
from __future__ import annotations

import logging
import os

from pydantic import Field

from fastapi import APIRouter

from khensu_services.common.api_models import RequestModel, ResponseModel
from khensu_services.common.errors import invalid_input, require_file
from khensu_services.common.executors import run_io_blocking

_log = logging.getLogger(__name__)
router = APIRouter()


class AutomationPointModel(RequestModel):
    time_sec: float = Field(ge=0.0)
    value: float  # plain parameter value (dB, Hz, ratio, ms; 0/1 for switches)


class AutomationLaneModel(RequestModel):
    param_id: str                      # a Khensu Copilot parameter id, e.g. "eq_gain_db"
    points: list[AutomationPointModel] = Field(default_factory=list)
    cc: int | None = Field(default=None, ge=0, le=119)  # None = next free undefined CC


class AutomationFixModel(RequestModel):
    # Recommendation suggested_parameters (eq_gain_db, comp_ratio,
    # reduce_gain_db, ...) or plugin parameter ids. Keys are data.
    parameters: dict[str, float] = Field(default_factory=dict)
    start_sec: float = Field(ge=0.0)
    end_sec: float = Field(gt=0.0)
    ramp_sec: float = Field(default=0.25, ge=0.0, le=10.0)
    # Current plugin values the fix ramps away from and back to.
    baseline: dict[str, float] = Field(default_factory=dict)


class AutomationExportRequest(RequestModel):
    output_dir: str
    name: str = "khensu_fix"
    fix: AutomationFixModel | None = None
    lanes: list[AutomationLaneModel] = Field(default_factory=list)
    bpm: float = Field(default=120.0, ge=20.0, le=400.0)
    midi_channel: int = Field(default=1, ge=1, le=16)
    resolution_ms: float = Field(default=20.0, ge=1.0, le=1000.0)
    write_midi: bool = True
    write_reaper: bool = True
    # The session audio the fix belongs to: its folder is an allowed root.
    source_path: str | None = None


class AutomationLaneSummary(ResponseModel):
    param_id: str
    parameter_index: int
    cc: int
    points: int
    start_sec: float
    end_sec: float


class AutomationExportResponse(ResponseModel):
    midi_path: str | None = None
    reaper_path: str | None = None
    lanes: list[AutomationLaneSummary]
    unmapped_parameters: list[str]
    duration_sec: float


@router.post("/automation/export", response_model=AutomationExportResponse,
             response_model_exclude_unset=True)
async def automation_export(req: AutomationExportRequest) -> dict:
    """Write a fix (or explicit curves) as MIDI CC lanes and REAPER envelopes."""
    from khensu_services.copilot import automation_export as ae
    from khensu_services.common.output_paths import (
        reject_output_alias, require_output_extension, validate_output_path,
    )

    if not req.write_midi and not req.write_reaper:
        invalid_input("Nothing to write: write_midi and write_reaper are both false")
    if req.fix is None and not req.lanes:
        invalid_input("Provide a fix or at least one lane")

    try:
        lanes: list[ae.Lane] = []
        unmapped: list[str] = []
        if req.fix is not None:
            fix_lanes, unmapped = ae.lanes_from_fix(req.fix.parameters, req.fix.start_sec, req.fix.end_sec,
                                                    req.fix.ramp_sec, req.fix.baseline)
            lanes.extend(fix_lanes)
        for lane_req in req.lanes:
            lane = ae.normalise_lane(lane_req.param_id, [(p.time_sec, p.value) for p in lane_req.points])
            lane.cc = lane_req.cc
            lanes.append(lane)
        seen: set[str] = set()
        for lane in lanes:
            if lane.param_id in seen:
                invalid_input(f"Two lanes automate {lane.param_id}")
            seen.add(lane.param_id)
        ae.assign_ccs(lanes)
    except ae.AutomationError as exc:
        invalid_input(str(exc))

    # source_path only widens the allowed write roots (its folder); it is
    # never decoded, but it must name a real local file or any folder could
    # be claimed as a root.
    if req.source_path:
        require_file(req.source_path, "Source file")

    stem = ae.safe_stem(req.name)
    midi_path = reaper_path = None
    if req.write_midi:
        midi_str = os.path.join(req.output_dir, stem + ".mid")
        require_output_extension(midi_str, (".mid",))
        midi_path = validate_output_path(midi_str, req.source_path)
        reject_output_alias(str(midi_path), req.source_path)
    if req.write_reaper:
        reaper_str = os.path.join(req.output_dir, stem + "_reaper_envelopes.txt")
        require_output_extension(reaper_str, (".txt",))
        reaper_path = validate_output_path(reaper_str, req.source_path)
        reject_output_alias(str(reaper_path), req.source_path)

    try:
        await run_io_blocking(ae.write_outputs, lanes, midi_path, reaper_path, bpm=req.bpm,
                              channel=req.midi_channel, resolution_sec=req.resolution_ms / 1000.0, name=req.name)
    except ae.AutomationError as exc:
        invalid_input(str(exc))

    response: dict = {
        "lanes": [
            {
                "param_id": lane.param_id,
                "parameter_index": lane.param.index,
                "cc": int(lane.cc or 0),
                "points": len(lane.points),
                "start_sec": lane.points[0][0],
                "end_sec": lane.points[-1][0],
            }
            for lane in lanes
        ],
        "unmapped_parameters": unmapped,
        "duration_sec": max(lane.points[-1][0] for lane in lanes),
    }
    if midi_path is not None:
        response["midi_path"] = str(midi_path)
    if reaper_path is not None:
        response["reaper_path"] = str(reaper_path)
    return response
