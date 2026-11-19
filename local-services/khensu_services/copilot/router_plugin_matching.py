"""Scanned-user-plugin lookup/matching endpoints (audit CQ-08: split out of
the copilot/router.py monolith into a bounded domain, same rationale as
router_model_downloads.py).

`EffectSpec` lives here rather than in router.py because router.py itself
depends on it (`ChatRequest`/`ApplyChainRequest`/etc. all reference it) —
defining it here and having router.py import it back keeps the dependency
one-directional instead of circular.

Mounted into `router.router` (which owns the `/copilot` prefix) via
`include_router()`, so this file's own routes stay unprefixed and the final
route surface is unchanged for callers and tests (including `from
khensu_services.copilot.router import EffectSpec`, which still works since
router.py re-exports it).
"""
from __future__ import annotations

from fastapi import APIRouter
from pydantic import Field, FiniteFloat

from khensu_services.common.api_models import RequestModel, ResponseModel
from khensu_services.copilot.matcher import resolve_effect_chain
from khensu_services.common.executors import io_route

router = APIRouter()


class EffectSpec(RequestModel):
    effect: str
    # ConvolutionReverb needs an impulse_response_path; all DSP numeric values
    # still pass through dsp._clamped_params before processing.
    parameters: dict[str, FiniteFloat | str] = Field(default_factory=dict, max_length=64)


class ResolveChainRequest(RequestModel):
    effects: list[EffectSpec]


class MatchedPluginSpec(ResponseModel):
    # khensu.db stores the id as TEXT, but older scans wrote INTEGER ids:
    # accept both.
    name: str
    manufacturer: str | None = None
    category: str | None = None
    format: str | None = None
    file_path: str | None = None
    id: int | str


class ResolvedEffectSpec(ResponseModel):
    effect: str
    # Same value type as EffectSpec.parameters (audit CPL-20/SVC-03): a
    # ConvolutionReverb impulse_response_path is a string, and declaring
    # dict[str, float] here turned every such request into a 500.
    parameters: dict[str, float | str]
    matched_plugin: MatchedPluginSpec | None = None


@router.post("/resolve_chain", response_model=list[ResolvedEffectSpec], response_model_exclude_unset=True)
@io_route
def resolve_chain(req: ResolveChainRequest):
    """Resolves a list of generic EffectSpecs to specific scanned user plugins."""
    effects_data = [e.model_dump() for e in req.effects]
    return resolve_effect_chain(effects_data)
