# local-services/khensu_services/analysis/router_take_alignment.py
from __future__ import annotations
import logging
import math
import os
from typing import Any

from fastapi import APIRouter
from khensu_services.common.errors import conflict, internal_error, invalid_input, not_found

from khensu_services.analysis.models import AlignTakesRequest, AlignTakesResponse
from khensu_services.analysis.take_alignment import VALID_GRID_DIVISIONS, render_suffix
from khensu_services.common.output_paths import reject_output_alias, resolve_render_output
from khensu_services.common.executors import run_blocking

_log = logging.getLogger(__name__)
router = APIRouter()

MAX_TAKES = 8


def _validate(req: AlignTakesRequest) -> None:
    if req.mode not in ("align", "quantize"):
        invalid_input("mode must be 'align' or 'quantize'")
    if not 1 <= len(req.take_paths) <= MAX_TAKES:
        invalid_input(f"take_paths must hold 1..{MAX_TAKES} files")
    if req.grid_division not in VALID_GRID_DIVISIONS:
        invalid_input("grid_division must be 4, 8, 16 or 32")
    if not (math.isfinite(req.strength) and 0.0 <= req.strength <= 1.0):
        invalid_input("strength must be within 0..1")
    if not (math.isfinite(req.max_shift_ms) and req.max_shift_ms > 0.0):
        invalid_input("max_shift_ms must be > 0")
    if req.bpm is not None and not (math.isfinite(req.bpm) and 20.0 <= req.bpm <= 400.0):
        invalid_input("bpm must be within 20..400")
    for p in [req.reference_path, *req.take_paths]:
        if not os.path.isfile(p):
            not_found(f"File not found: {p}")


@router.post("/analyze/align_takes", response_model=AlignTakesResponse, response_model_exclude_unset=True)
async def align_takes_endpoint(req: AlignTakesRequest) -> dict[str, Any]:
    _validate(req)
    # Every derived render path goes through the same allowed-roots +
    # no-source-alias gate as the sibling render endpoints, before any work.
    outs: list[str] | None = None
    if req.render:
        suffix = render_suffix(req.mode)
        # UIX-11: caller-chosen render paths (one per take, e.g. from
        # uniqueRenderFile) so a new render never overwrites a file the render
        # history still references; absent, <take>_<suffix>.wav beside each.
        wanted = list(req.output_paths) if req.output_paths else [None] * len(req.take_paths)
        if len(wanted) != len(req.take_paths):
            invalid_input("output_paths must hold one path per take")
        outs = [resolve_render_output(w, p, suffix, allowed_exts=(".wav",))
                for w, p in zip(wanted, req.take_paths)]
        if len({os.path.normcase(os.path.abspath(o)) for o in outs}) != len(outs):
            conflict("output_paths must be distinct")
        for out in outs:
            reject_output_alias(out, req.reference_path, *req.take_paths)

    from khensu_services.analysis.take_alignment import align_takes
    try:
        return await run_blocking(align_takes, req.reference_path, list(req.take_paths),
                                  req.mode, req.bpm, req.grid_division, req.strength,
                                  req.max_shift_ms, req.render, outs)
    except ValueError as exc:
        invalid_input(str(exc), cause=exc)
    except Exception as exc:
        _log.exception("Take alignment failed")
        internal_error(cause=exc)
