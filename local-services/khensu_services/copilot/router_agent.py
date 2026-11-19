# local-services/khensu_services/copilot/router_agent.py
"""Agentic mixing endpoints (CE-AG-1).

    POST /agent/mix                     start the verified auto-mix job
    GET  /agent/mix/{job_id}            status, with the live step log
    POST /agent/mix/{job_id}/cancel
    POST /agent/findings                measured findings for a file or session
    GET  /agent/sessions                sessions in khensu.db (for MCP clients)

The loop itself is ``copilot/agent_loop.py``; every render goes through the
shared pipeline in ``router_render`` (validator, quarantine, post-render
guardrail, promote), and every kept step is logged as a provenance event.
"""
from __future__ import annotations

import json
import logging
import math
import os
import sqlite3

from fastapi import APIRouter
from pydantic import Field

from khensu_services.common.api_models import RequestModel, ResponseModel
from khensu_services.common.errors import invalid_input, not_found, require_decode_budget, require_file
from khensu_services.common.executors import long_job_executor, run_blocking, run_io_blocking
from khensu_services.common.job_models import JobCancelResponse, JobIdResponse
from khensu_services.common.job_status import JobStatus
from khensu_services.common.jobs import (
    JobStore, cancel_reply, get_job_or_404, job_begin, job_cancel_requested, job_settle,
    job_update, request_fingerprint, start_job,
)
from khensu_services.copilot import agent_loop

_log = logging.getLogger(__name__)
router = APIRouter()


# ---------------------------------------------------------------------------
# Models
# ---------------------------------------------------------------------------
class AgentFindingIn(RequestModel):
    """A desktop recommendation to act on (its engine's vocabulary)."""
    id: str = ""
    title: str = ""
    what: str = ""
    reason: str = ""
    metric: str = ""                  # optional: the tracked metric it moves
    severity: float = 0.0
    suggested_parameters: dict[str, float] = Field(default_factory=dict)


class AgentMixRequest(RequestModel):
    file_path: str
    # Where candidate renders go; default "<source>_automix" beside the source.
    output_dir: str = ""
    session_id: str = ""
    # Desktop findings; when empty the agent measures and derives its own.
    findings: list[AgentFindingIn] = Field(default_factory=list, max_length=24)
    # Per-metric acceptable ranges {metric: [lo, hi]} (e.g. from the genre).
    targets: dict[str, list[float]] = Field(default_factory=dict)
    target_lufs: float = Field(default=-14.0, ge=-40.0, le=-3.0)
    include_loudness: bool = True
    max_steps: int = Field(default=6, ge=1, le=12)


class AgentFindingsRequest(RequestModel):
    file_path: str = ""
    session_id: str = ""
    targets: dict[str, list[float]] = Field(default_factory=dict)
    target_lufs: float = Field(default=-14.0, ge=-40.0, le=-3.0)
    include_loudness: bool = True


class AgentFinding(ResponseModel):
    id: str
    metric: str = ""
    title: str = ""
    measured: float | None = None
    range: list[float] | None = None
    reason: str = ""
    suggested_parameters: dict[str, float] = {}
    severity: float = 0.0
    source: str = ""


class AgentEffect(ResponseModel):
    effect: str
    parameters: dict[str, float] = {}


class AgentComparison(ResponseModel):
    metric: str
    before: float | None = None
    after: float | None = None
    improvement: float | None = None
    targeted: bool = False


class AgentGuardrail(ResponseModel):
    ok: bool | None = None
    matched_gain_db: float | None = None
    blocks: list[str] | None = None
    warnings: list[str] | None = None


class AgentAttempt(ResponseModel):
    strength: float
    effects: list[AgentEffect] = []
    output_path: str = ""
    decision: str = ""
    reason: str = ""
    after: dict[str, float | None] | None = None
    matched_gain_db: float | None = None
    comparison: list[AgentComparison] = []
    guardrail: AgentGuardrail | None = None


class AgentStep(ResponseModel):
    index: int
    finding_id: str
    title: str = ""
    metric: str = ""
    reason: str = ""
    decision: str                   # kept | reverted | skipped
    decision_reason: str = ""
    output_path: str = ""
    before: dict[str, float | None] | None = None
    after: dict[str, float | None] | None = None
    effects: list[AgentEffect] = []
    strength: float | None = None
    matched_gain_db: float | None = None
    attempts: list[AgentAttempt] = []


class AgentMixResult(ResponseModel):
    source_path: str
    final_path: str
    changed: bool
    kept_count: int
    reverted_count: int
    skipped_count: int = 0
    steps: list[AgentStep] = []
    findings: list[AgentFinding] = []
    before: dict[str, float | None] = {}
    after: dict[str, float | None] = {}
    kept_paths: list[str] = []
    remaining_out_of_range: list[str] = []
    ranges: dict[str, list[float]] = {}
    summary: str = ""


class AgentProgress(ResponseModel):
    stage: str = ""
    current_path: str = ""
    findings: list[AgentFinding] = []
    steps: list[AgentStep] = []
    before: dict[str, float | None] = {}


class AgentMixStatusResponse(ResponseModel):
    status: JobStatus
    stage: str | None = None
    progress: AgentProgress | None = None
    result: AgentMixResult | None = None
    error: str | None = None


class AgentFindingsResponse(ResponseModel):
    file_path: str
    metrics: dict[str, float | None] = {}
    findings: list[AgentFinding] = []
    ranges: dict[str, list[float]] = {}
    session_id: str = ""


class AgentSession(ResponseModel):
    id: str
    name: str = ""
    created_at: str = ""
    updated_at: str = ""
    file_paths: list[str] = []


class AgentSessionsResponse(ResponseModel):
    sessions: list[AgentSession] = []
    db_path: str = ""


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
def json_safe(obj):
    """Non-finite floats -> None (JSON has no NaN); recursive."""
    if isinstance(obj, float):
        return obj if math.isfinite(obj) else None
    if isinstance(obj, dict):
        return {k: json_safe(v) for k, v in obj.items()}
    if isinstance(obj, (list, tuple)):
        return [json_safe(v) for v in obj]
    return obj


def _default_out_dir(source_path: str) -> str:
    base, _ext = os.path.splitext(source_path)
    return base + "_automix"


def _render_step_fn(job_id: str):
    """render_step for agent_loop: the shared pipeline, cancellable."""
    from khensu_services.copilot.dsp import apply_pedalboard_chain
    from khensu_services.copilot.router_render import render_through_pipeline

    def render_step(source: str, output: str, effects: list[dict]) -> dict:
        _result, guardrail, _validation = render_through_pipeline(
            source, output,
            lambda quarantine: apply_pedalboard_chain(source, quarantine, effects, normalize=False),
            effects=effects, graph=None,
            should_cancel=lambda: job_cancel_requested(_agent_jobs, job_id))
        return guardrail
    return render_step


def _log_event(event_type: str, session_id: str, rec_id: str, path: str, applied: list[str], chain) -> None:
    from khensu_services.copilot.router_render import _log_applied_event
    _log_applied_event(event_type, session_id, rec_id, path, applied, chain)


# ---------------------------------------------------------------------------
# /agent/mix job
# ---------------------------------------------------------------------------
_agent_jobs = JobStore(name="agent_mix", max_active=2)
_agent_futures: dict = {}


def _run_agent_job(job_id: str, req: AgentMixRequest, out_dir: str) -> None:
    if not job_begin(_agent_jobs, job_id):
        return

    def on_progress(state: dict) -> None:
        job_update(_agent_jobs, job_id, stage=state.get("stage", ""), progress=json_safe({
            "stage": state.get("stage", ""), "current_path": state.get("current_path", ""),
            "findings": state.get("findings", []), "steps": state.get("steps", []),
            "before": state.get("before", {}),
        }))

    def on_kept(step: dict) -> None:
        _log_event("agent_step_kept", req.session_id, step["finding_id"], step["output_path"],
                   [e["effect"] for e in step.get("effects", [])],
                   {"effects": step.get("effects", []), "reason": step["decision_reason"],
                    "metric": step["metric"], "before": json_safe(step["before"]),
                    "after": json_safe(step["after"])})

    try:
        os.makedirs(out_dir, exist_ok=True)
        result = agent_loop.run_agent(
            req.file_path, out_dir, job_tag=job_id[:8],
            findings=[f.model_dump() for f in req.findings],
            targets=req.targets, target_lufs=req.target_lufs,
            include_loudness=req.include_loudness, max_steps=req.max_steps,
            render_step=_render_step_fn(job_id), on_progress=on_progress,
            should_cancel=lambda: job_cancel_requested(_agent_jobs, job_id), on_kept=on_kept)
        if result["changed"]:
            _log_event("agent_mix_completed", req.session_id, job_id, result["final_path"],
                       [s["title"] for s in result["steps"] if s["decision"] == "kept"],
                       {"summary": result["summary"], "kept_paths": result["kept_paths"]})
        job_settle(_agent_jobs, job_id, status=JobStatus.COMPLETED.value, stage="done",
                   result=json_safe(result))
    except agent_loop.AgentCancelled:
        job_settle(_agent_jobs, job_id, status=JobStatus.CANCELLED.value, stage="cancelled")
    except Exception as exc:
        _log.exception("Agent mix failed for job %s", job_id)
        job_settle(_agent_jobs, job_id, status=JobStatus.FAILED.value, error=str(exc))


@router.post("/agent/mix", response_model=JobIdResponse, response_model_exclude_unset=True)
async def agent_mix_start(req: AgentMixRequest) -> dict:
    """Start the verified auto-mix: plan from the findings, render each step,
    re-measure at matched loudness, keep only what improved its target
    without making anything else worse and passed the guardrail."""
    require_file(req.file_path, "Audio file")
    await require_decode_budget(req.file_path)
    for key, rng in req.targets.items():
        if key not in agent_loop.METRICS:
            invalid_input(f"Unknown target metric '{key}'. Known: {sorted(agent_loop.METRICS)}")
        if len(rng) != 2 or not all(math.isfinite(v) for v in rng) or rng[0] > rng[1]:
            invalid_input(f"Target '{key}' must be [lo, hi] with lo <= hi.")
    from khensu_services.common.output_paths import validate_output_path
    out_dir = (req.output_dir or "").strip() or _default_out_dir(req.file_path)
    # Validate the folder through a probe file name inside it (same roots
    # as every render output: home, temp, the source's folder).
    validate_output_path(os.path.join(out_dir, "agent_probe.wav"), req.file_path)
    if not any(agent_loop.chain_from_suggested_parameters(f.suggested_parameters) for f in req.findings) \
            and req.findings:
        invalid_input("None of the findings carry renderable suggested_parameters.")
    st = os.stat(req.file_path)
    dedup = f"{req.file_path}|{request_fingerprint(req, [st.st_mtime_ns, st.st_size])}"
    job_id, _created = start_job(
        _agent_jobs, long_job_executor, _run_agent_job, req, out_dir,
        payload={"status": JobStatus.PENDING.value, "stage": "queued", "progress": None,
                 "result": None, "error": None},
        dedup_key=dedup, futures=_agent_futures)
    return {"job_id": job_id}


@router.get("/agent/mix/{job_id}", response_model=AgentMixStatusResponse, response_model_exclude_unset=True)
async def agent_mix_status(job_id: str) -> dict:
    return get_job_or_404(_agent_jobs, job_id)


@router.post("/agent/mix/{job_id}/cancel", response_model=JobCancelResponse, response_model_exclude_unset=True)
async def agent_mix_cancel(job_id: str) -> dict:
    return cancel_reply(_agent_jobs, job_id, _agent_futures)


# ---------------------------------------------------------------------------
# Findings and sessions (also the MCP server's read tools)
# ---------------------------------------------------------------------------
def _khensu_db_path() -> str:
    override = os.environ.get("KHENSU_SESSIONS_DB", "").strip()
    if override:
        return override
    from khensu_services.auth import get_token_path
    return str(get_token_path().parent / "khensu.db")


def list_sessions(limit: int = 100) -> dict:
    """Sessions from khensu.db (read-only; the desktop owns the table)."""
    path = _khensu_db_path()
    if not os.path.isfile(path):
        return {"sessions": [], "db_path": path}
    uri = "file:" + path.replace("\\", "/") + "?mode=ro"
    try:
        conn = sqlite3.connect(uri, uri=True, timeout=5.0)
    except sqlite3.Error:
        return {"sessions": [], "db_path": path}
    try:
        try:
            rows = conn.execute("SELECT id, name, created_at, updated_at, data_json FROM sessions "
                                "ORDER BY updated_at DESC LIMIT ?", (int(limit),)).fetchall()
        except sqlite3.Error:
            return {"sessions": [], "db_path": path}
    finally:
        conn.close()
    sessions = []
    for sid, name, created, updated, data_json in rows:
        files: list[str] = []
        try:
            data = json.loads(data_json or "{}")
            for f in data.get("files", []) if isinstance(data, dict) else []:
                if isinstance(f, dict) and isinstance(f.get("path"), str):
                    files.append(f["path"])
        except ValueError:
            pass
        sessions.append({"id": str(sid), "name": name or "", "created_at": created or "",
                         "updated_at": updated or "", "file_paths": files})
    return {"sessions": sessions, "db_path": path}


@router.get("/agent/sessions", response_model=AgentSessionsResponse, response_model_exclude_unset=True)
async def agent_sessions(limit: int = 100) -> dict:
    return await run_io_blocking(list_sessions, max(1, min(limit, 500)))


def _findings(file_path: str, targets: dict, target_lufs: float, include_loudness: bool) -> dict:
    specs = agent_loop.apply_targets(targets)
    metrics = agent_loop.measure(file_path)
    findings = agent_loop.derive_findings(metrics, specs, target_lufs, include_loudness)
    return json_safe({"file_path": file_path, "metrics": metrics, "findings": findings,
                      "ranges": {k: [s.lo, s.hi] for k, s in specs.items()}})


@router.post("/agent/findings", response_model=AgentFindingsResponse, response_model_exclude_unset=True)
async def agent_findings(req: AgentFindingsRequest) -> dict:
    """Measured findings for ``file_path``, or for a session's first file."""
    file_path = req.file_path
    if not file_path and req.session_id:
        listing = await run_io_blocking(list_sessions, 500)
        match = next((s for s in listing["sessions"] if s["id"] == req.session_id), None)
        if match is None:
            not_found(f"Session not found: {req.session_id}")
        if not match["file_paths"]:
            not_found("The session has no audio file.")
        file_path = match["file_paths"][0]
    if not file_path:
        invalid_input("Give file_path or session_id.")
    require_file(file_path, "Audio file")
    await require_decode_budget(file_path)
    out = await run_blocking(_findings, file_path, req.targets, req.target_lufs, req.include_loudness)
    if req.session_id:
        out["session_id"] = req.session_id
    return out
