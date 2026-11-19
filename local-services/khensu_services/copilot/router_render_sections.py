"""Section-aware chain renders and mixer-chain generation jobs (ENG-6, from router_render.py)."""
from __future__ import annotations

import logging
import math
import os
from fastapi import APIRouter
from pydantic import Field, field_validator, model_validator
from khensu_services.common.api_models import RequestModel
from khensu_services.common.job_status import JobStatus
from khensu_services.copilot.dsp import apply_section_chains
from khensu_services.common.errors import conflict, invalid_input, not_found
from khensu_services.common.jobs import JobStore, get_job_or_404, job_begin, job_cancel_requested, job_settle, request_fingerprint, start_job
from khensu_services.common.executors import long_job_executor as _long_executor, llm_executor as _llm_executor, run_blocking, run_llm_blocking
from khensu_services.copilot.mixer_chain_generator import generate_mixer_chain_response, MixerChainResponse
from khensu_services.copilot.router_plugin_matching import EffectSpec
from khensu_services.copilot.response_models import CopilotJobCancelResponse, JobStartResponse, MixerChainJobStatusResponse, RenderJobStatusResponse
from khensu_services.copilot.router_render import MixerChainRequest, PostRenderGuardrailError, RENDER_STATE_BLOCKED, RENDER_STATE_VALIDATED, RenderBlocked, RenderCancelled, _cancel_reply, _decision_hash_field, _effect_dicts, _file_identity, _genre_action_block, _log_applied_event, _rejected_job_result, _require_audio_decode_budget, render_through_pipeline

_log = logging.getLogger(__name__)
router = APIRouter()


# ---------------------------------------------------------------------------
# Section-aware processing — a different (or absent) effect chain per
# detected song section, so a chorus/drop can carry more compression/
# saturation intensity than a verse instead of one chain for the whole
# track. Job-queued: two renders' worth of DSP work (per-section, stitched)
# can take a while on longer files, same reasoning as apply_chain/start.
# ---------------------------------------------------------------------------
class SectionChainSpec(RequestModel):
    label: str = ""
    start_sec: float
    end_sec: float
    effects: list[EffectSpec] = Field(default_factory=list, max_length=64)

    @field_validator("start_sec", "end_sec")
    @classmethod
    def _require_finite_time(cls, value: float) -> float:
        if not math.isfinite(value):
            raise ValueError("Section times must be finite")
        return value

    @model_validator(mode="after")
    def _require_positive_span(self) -> "SectionChainSpec":
        if self.start_sec < 0.0 or self.end_sec <= self.start_sec:
            raise ValueError("end_sec must be greater than non-negative start_sec")
        return self


class SectionAwareChainRequest(RequestModel):
    file_path: str
    output_path: str
    sections: list[SectionChainSpec] = Field(max_length=200)
    crossfade_ms: float = 30.0
    session_id: str = ""
    source_recommendation_id: str = ""
    # See ApplyChainRequest.decision_hash.
    decision_hash: str = _decision_hash_field()
    genre_relative: bool = False
    genre_action_allowed: bool = False
    abstained_reason: str = ""
    target_stem: str = "mix"
    stem_paths: dict[str, str] = Field(default_factory=dict)

    @model_validator(mode="after")
    def _require_ordered_non_overlapping_sections(self) -> "SectionAwareChainRequest":
        previous_end = 0.0
        for section in self.sections:
            if section.start_sec < previous_end:
                raise ValueError("Sections must be ordered and non-overlapping")
            previous_end = section.end_sec
        return self


_section_chain_jobs = JobStore(name="section_chain", max_active=4)  # bounded (F2) + admission cap (ARCH-01)


_section_futures: dict = {}


def _section_dicts(req: SectionAwareChainRequest) -> list[dict]:
    return [
        {"label": s.label, "start_sec": s.start_sec, "end_sec": s.end_sec,
         "effects": _effect_dicts(s.effects)}
        for s in req.sections
    ]


def _run_section_chain_job(job_id: str, req: SectionAwareChainRequest) -> None:
    if not job_begin(_section_chain_jobs, job_id):
        return
    try:
        sections = _section_dicts(req)
        flat = [e for sec in sections for e in sec["effects"]]
        # validate -> quarantine -> render -> guardrail -> promote (S2). The
        # flattened section effects are validated exactly as at /start, so a
        # source swapped between admission and render is still checked.
        result, guardrail, _validation = render_through_pipeline(
            req.file_path, req.output_path,
            lambda quarantine: apply_section_chains(req.file_path, quarantine, sections, req.crossfade_ms),
            effects=flat, graph=None,
            should_cancel=lambda: job_cancel_requested(_section_chain_jobs, job_id))
        result["guardrail"] = guardrail
        result["output_path"] = req.output_path
        result["status"] = "success"
        result["render_state"] = RENDER_STATE_VALIDATED
        _log_applied_event("section_chain_applied", req.session_id, req.source_recommendation_id,
                           req.output_path, [r["label"] for r in result.get("sections", [])], sections,
                           req.decision_hash)
        if req.decision_hash:
            result["decision_hash"] = req.decision_hash
        job_settle(_section_chain_jobs, job_id, status=JobStatus.COMPLETED.value, result=result)
    except RenderCancelled:
        return
    except RenderBlocked as blocked:
        job_settle(_section_chain_jobs, job_id, status=JobStatus.FAILED.value, error=str(blocked),
                   result={"status": "blocked", "render_state": RENDER_STATE_BLOCKED,
                           "guardrail": {"pre_render": blocked.validation}})
    except PostRenderGuardrailError as exc:
        _log.warning("Section-chain render rejected by post-render guardrail for job %s: %s", job_id, exc)
        job_settle(_section_chain_jobs, job_id, status=JobStatus.FAILED.value, error=str(exc),
                   result=_rejected_job_result(exc, req.file_path, req.output_path))
    except Exception as exc:
        _log.exception("Background section-aware chain failed for job %s", job_id)
        job_settle(_section_chain_jobs, job_id, status=JobStatus.FAILED.value, error=str(exc))


@router.post("/apply_section_chain/start", response_model=JobStartResponse, response_model_exclude_unset=True)
async def apply_section_chain_start(req: SectionAwareChainRequest) -> dict:
    blocked_reason = _genre_action_block(req)
    if blocked_reason:
        conflict(blocked_reason)
    from khensu_services.copilot.render_validator import validate_stem_attribution
    attribution = validate_stem_attribution(req.file_path, req.target_stem, req.stem_paths)
    if not attribution["ok"]:
        conflict("; ".join(attribution["blocks"]))
    if not os.path.isfile(req.file_path):
        not_found(f"File not found: {req.file_path}")
    await _require_audio_decode_budget(req.file_path)
    if not req.sections:
        invalid_input("At least one section is required")

    # The section path was previously outside apply_chain's preflight. Flatten
    # its effects so mono/layout/headroom/widening risks block before render.
    from khensu_services.copilot.render_validator import InvalidRenderParameter, validate_render_request
    effects = [{"effect": effect.effect, "parameters": effect.parameters}
               for section in req.sections for effect in section.effects]
    try:
        validation = await run_blocking(validate_render_request, req.file_path, effects, None)
    except InvalidRenderParameter as exc:
        invalid_input(str(exc))
    if not validation["ok"]:
        conflict("; ".join(validation["blocks"]))

    from khensu_services.common.output_paths import validate_output_path, reject_output_alias
    validate_output_path(req.output_path, req.file_path)
    reject_output_alias(req.output_path, req.file_path, *req.stem_paths.values())

    # Dedup (audit ARCH-01): same (source, destination) joins the
    # already-running section-aware render instead of starting a second one -
    # while the source is unchanged (mtime+size, SVC-07).
    dedup_key = (f"{req.file_path}|{req.output_path}|"
                 f"{request_fingerprint(req, _file_identity(req.file_path, *req.stem_paths.values()))}")
    job_id, _created = start_job(
        _section_chain_jobs, _long_executor, _run_section_chain_job, req,
        payload={"status": JobStatus.PENDING.value, "result": None, "error": None}, dedup_key=dedup_key,
        futures=_section_futures)
    return {"job_id": job_id}


@router.get("/apply_section_chain/status/{job_id}", response_model=RenderJobStatusResponse, response_model_exclude_unset=True)
async def apply_section_chain_status(job_id: str) -> dict:
    return get_job_or_404(_section_chain_jobs, job_id)


@router.post("/apply_section_chain/{job_id}/cancel", response_model=CopilotJobCancelResponse, response_model_exclude_unset=True)
async def apply_section_chain_cancel(job_id: str) -> dict:
    return _cancel_reply(_section_chain_jobs, job_id, _section_futures)


@router.post("/generate_mixer_chain", response_model=MixerChainResponse, response_model_exclude_unset=True)
async def generate_mixer_chain_endpoint(req: MixerChainRequest) -> MixerChainResponse:
    """Mixer-chain generation is an LLM round-trip: run it on the LLM pool."""
    return await run_llm_blocking(generate_mixer_chain, req)


def generate_mixer_chain(req: MixerChainRequest) -> MixerChainResponse:
    """Generates a custom plugin mixer insert chain based on target channel, song context, and enabled ecosystems."""
    return generate_mixer_chain_response(
        target_channel=req.target_channel,
        genre=req.genre,
        energy=req.energy,
        mood=req.mood,
        bpm=req.bpm,
        style_description=req.style_description,
        enabled_ecosystems=req.enabled_ecosystems,
        vocal_strategy=req.vocal_strategy,
        vocal_macros=req.vocal_macros,
        preference_lufs_offset=req.preference_lufs_offset,
        preference_lra_offset=req.preference_lra_offset,
        preference_stereo_width_offset=req.preference_stereo_width_offset,
        preference_low_end_weight_offset=req.preference_low_end_weight_offset,
    )


# ---------------------------------------------------------------------------
# Job-queue variant — mixer-chain generation is an LLM round-trip (the other
# named case in the audit's "stem-sep + LLM ops" job-queue finding) that can
# hang on a slow/unresponsive API; the plain endpoint above has no way for a
# client to cancel or check progress on a request already in flight.
# ---------------------------------------------------------------------------
_mixer_chain_jobs = JobStore(name="mixer_chain", max_active=8)  # bounded (F2) + admission cap (ARCH-01)


_mixer_chain_futures: dict = {}


def _run_mixer_chain_job(job_id: str, req: MixerChainRequest) -> None:
    if not job_begin(_mixer_chain_jobs, job_id):
        return
    try:
        result = generate_mixer_chain_response(
            target_channel=req.target_channel, genre=req.genre, energy=req.energy,
            mood=req.mood, bpm=req.bpm, style_description=req.style_description,
            enabled_ecosystems=req.enabled_ecosystems, vocal_strategy=req.vocal_strategy,
            vocal_macros=req.vocal_macros,
            preference_lufs_offset=req.preference_lufs_offset,
            preference_lra_offset=req.preference_lra_offset,
            preference_stereo_width_offset=req.preference_stereo_width_offset,
            preference_low_end_weight_offset=req.preference_low_end_weight_offset,
        )
        job_settle(_mixer_chain_jobs, job_id, status=JobStatus.COMPLETED.value, result=result.model_dump())
    except Exception as exc:
        _log.exception("Background mixer-chain generation failed for job %s", job_id)
        job_settle(_mixer_chain_jobs, job_id, status=JobStatus.FAILED.value, error=str(exc))


@router.post("/generate_mixer_chain/start", response_model=JobStartResponse, response_model_exclude_unset=True)
async def generate_mixer_chain_start(req: MixerChainRequest) -> dict:
    job_id, _created = start_job(_mixer_chain_jobs, _llm_executor, _run_mixer_chain_job, req,
                                 payload={"status": JobStatus.PENDING.value, "result": None, "error": None},
                                 futures=_mixer_chain_futures)
    return {"job_id": job_id}


@router.get("/generate_mixer_chain/status/{job_id}", response_model=MixerChainJobStatusResponse, response_model_exclude_unset=True)
async def generate_mixer_chain_status(job_id: str) -> dict:
    return get_job_or_404(_mixer_chain_jobs, job_id)


@router.post("/generate_mixer_chain/{job_id}/cancel", response_model=CopilotJobCancelResponse, response_model_exclude_unset=True)
async def generate_mixer_chain_cancel(job_id: str) -> dict:
    return _cancel_reply(_mixer_chain_jobs, job_id, _mixer_chain_futures)
