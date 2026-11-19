# local-services/khensu_services/copilot/router_mastering_agent.py
"""Mastering-agent job endpoints (audit CQ-08 split from copilot/router.py).

The iterative render->critique mastering loop as a background job:
/mastering_agent/start | /status/{id} | /{id}/cancel. Every iteration is
guardrail-checked inside run_mastering_agent, and the FINAL candidate is
re-checked here - a failed guardrail never auto-promotes (audit AI-01).
"""
from __future__ import annotations

import logging
import os

from fastapi import APIRouter
from pydantic import Field

from khensu_services.common.api_models import RequestModel
from khensu_services.common.job_status import JobStatus

from khensu_services.common.errors import conflict, require_decode_budget, require_file
from khensu_services.common.executors import long_job_executor as _executor
from khensu_services.common.jobs import (
    JobStore, cancel_reply, get_job_or_404, job_begin, job_cancel_requested, job_settle,
    request_fingerprint, start_job,
)
from khensu_services.copilot.response_models import (
    CopilotJobCancelResponse, JobStartResponse, MasteringAgentStatusResponse,
)
from khensu_services.copilot.router_plugin_matching import EffectSpec

_log = logging.getLogger(__name__)
router = APIRouter()


class MasteringAgentRequest(RequestModel):
    file_path: str
    genre_id: str = ""
    initial_chain: list[EffectSpec] = Field(default_factory=list, max_length=64)
    max_iterations: int = Field(default=5, ge=1, le=20)
    genre_action_allowed: bool = False
    abstained_reason: str = ""
    target_stem: str = "mix"
    stem_paths: dict[str, str] = Field(default_factory=dict)


_mastering_jobs = JobStore(name="mastering_agent", max_active=4)  # bounded (F2) + admission cap (ARCH-01)
_mastering_futures: dict = {}


def _lookup_genre(genre_id: str) -> dict:
    import json
    from pathlib import Path
    try:
        targets_path = Path(__file__).resolve().parents[3] / "resources" / "genre_norms" / "genre_targets.json"
        data = json.loads(targets_path.read_text(encoding="utf-8"))
        for g in data.get("genres", []):
            if g.get("id") == genre_id:
                return g
    except Exception:
        _log.warning("Could not load genre_targets.json for mastering agent", exc_info=True)
    # No explicit crest target: mastering_agent.crest_target_db derives it the
    # way the C++ GenreDatabase does (AUDIT-054), instead of a contradicting 10.0.
    return {"id": genre_id, "lufs_target": -12.0, "spectral_centroid_hz": 2500.0}


def _run_mastering_job(job_id: str, req: MasteringAgentRequest) -> None:
    from khensu_services.copilot.mastering_agent import run_mastering_agent

    if not job_begin(_mastering_jobs, job_id):
        return
    try:
        # Per-job directory (AUDIT-048): iteration files have fixed names, so
        # two concurrent runs (e.g. different genres on one file) sharing a
        # directory overwrote each other's candidates mid-run.
        out_root = os.path.join(os.path.dirname(req.file_path) or ".", "mastering_agent_output")
        out_dir = os.path.join(out_root, job_id)
        os.makedirs(out_dir, exist_ok=True)
        # Retention (SVC-10 / CPL-19): earlier runs' iteration WAVs are swept
        # once they are older than the retention window; this run's are kept.
        from khensu_services.copilot.render_retention import sweep_stale_job_dirs
        sweep_stale_job_dirs(out_root, keep={job_id})
        genre = _lookup_genre(req.genre_id)
        chain = [{"effect": e.effect, "parameters": e.parameters} for e in req.initial_chain]

        result = run_mastering_agent(
            req.file_path, out_dir, genre, initial_chain=chain,
            max_iterations=req.max_iterations,
            should_cancel=lambda: job_cancel_requested(_mastering_jobs, job_id),
        )
        output = result.get("best_safe_output_path")
        if output and os.path.isfile(output):
            from khensu_services.copilot.mastering_agent import _guardrail_failed
            from khensu_services.copilot.render_guardrails import post_render_guardrail
            try:
                result["guardrail"] = post_render_guardrail(req.file_path, output)
            except Exception as exc:   # fail closed: an unchecked render is not "safe"
                _log.exception("Final mastering guardrail failed for job %s", job_id)
                result["guardrail"] = _guardrail_failed(exc)
            # A failed guardrail never becomes an auto-promoted candidate.
            if not result["guardrail"].get("ok", False):
                result["requires_user_choice"] = True
                result["best_safe_output_path"] = ""
                result["final_output_path"] = ""
        job_settle(_mastering_jobs, job_id, status=JobStatus.COMPLETED.value, result=result)
    except Exception as exc:
        _log.exception("Background mastering agent failed for job %s", job_id)
        job_settle(_mastering_jobs, job_id, status=JobStatus.FAILED.value, error=str(exc))


@router.post("/mastering_agent/start", response_model=JobStartResponse, response_model_exclude_unset=True)
async def mastering_agent_start(req: MasteringAgentRequest) -> dict:
    if req.genre_id and not req.genre_action_allowed:
        conflict(req.abstained_reason or
                 "Mastering agent abstained: confirm genre targets or provide a reference track.")
    from khensu_services.copilot.render_validator import validate_stem_attribution
    attribution = validate_stem_attribution(req.file_path, req.target_stem, req.stem_paths)
    if not attribution["ok"]:
        conflict("; ".join(attribution["blocks"]))
    require_file(req.file_path)
    await require_decode_budget(req.file_path)
    # Dedup (audit ARCH-01): a double-click on the same file joins the
    # already-running mastering pass instead of starting a second one — but
    # only when every parameter (genre, chain, iterations, ...) matches.
    # The source's mtime+size is part of the key (SVC-07): a file re-rendered
    # in place mid-run starts a new job instead of joining the stale one.
    st = os.stat(req.file_path)
    dedup_key = f"{req.file_path}|{request_fingerprint(req, st.st_mtime_ns, st.st_size)}"
    job_id, _created = start_job(
        _mastering_jobs, _executor, _run_mastering_job, req,
        payload={"status": JobStatus.PENDING.value, "result": None, "error": None}, dedup_key=dedup_key, futures=_mastering_futures)
    return {"job_id": job_id}


@router.get("/mastering_agent/status/{job_id}", response_model=MasteringAgentStatusResponse, response_model_exclude_unset=True)
async def mastering_agent_status(job_id: str) -> dict:
    return get_job_or_404(_mastering_jobs, job_id)


@router.post("/mastering_agent/{job_id}/cancel", response_model=CopilotJobCancelResponse, response_model_exclude_unset=True)
async def mastering_agent_cancel(job_id: str) -> dict:
    return cancel_reply(_mastering_jobs, job_id, _mastering_futures)   # SVC-14
