# local-services/khensu_services/copilot/router_command.py
"""Natural-language and voice mix commands (CE-AG-4).

    POST /command/parse                  text -> parsed commands + render plan (preview)
    POST /command/execute                start a render job for a previewed plan
    GET  /command/execute/{job_id}       job status
    POST /command/execute/{job_id}/cancel
    POST /command/voice                  {audio_path} -> faster-whisper -> parse

Parse is the confirm step: it returns a ``plan_id`` for the exact plan the
user saw, and execute renders that stored plan (never a re-interpretation),
refusing it when the source file changed since the preview.
"""
from __future__ import annotations

import logging
import os
import threading
import time
import uuid

from fastapi import APIRouter
from pydantic import Field

from khensu_services.common.api_models import RequestModel, ResponseModel
from khensu_services.common.errors import (
    conflict,
    invalid_input,
    not_found,
    require_decode_budget,
    require_file,
    unavailable,
)
from khensu_services.common.executors import (
    long_job_executor, run_blocking, run_llm_blocking,
)
from khensu_services.common.job_models import JobCancelResponse, JobIdResponse
from khensu_services.common.job_status import JobStatus
from khensu_services.common.jobs import (
    JobStore, cancel_reply, get_job_or_404, job_begin, job_cancel_requested, job_settle,
    job_update, start_job,
)
from khensu_services.copilot import nl_commands as nl
from khensu_services.copilot.response_models import RenderGuardrail

_log = logging.getLogger(__name__)
router = APIRouter()

_MAX_TEXT = 500
_PLAN_TTL_SEC = 30 * 60
_MAX_PLANS = 64
_plans: dict[str, dict] = {}
_plans_lock = threading.Lock()


# ---------------------------------------------------------------------------
# Models
# ---------------------------------------------------------------------------
class CommandSectionIn(RequestModel):
    """A caller-labelled section (optional; otherwise the analyzer's)."""
    label: str = ""
    role: str = ""
    start_sec: float
    end_sec: float


class CommandParseRequest(RequestModel):
    text: str = Field(min_length=1, max_length=_MAX_TEXT)
    # The audio the command applies to. Without it the commands are parsed
    # but section targets and relative levels stay unresolved.
    file_path: str = ""
    stem_paths: dict[str, str] = Field(default_factory=dict)
    sections: list[CommandSectionIn] = Field(default_factory=list, max_length=200)
    allow_llm: bool = True


class PlanEffect(ResponseModel):
    effect: str
    parameters: dict[str, float] = {}


class PlanSection(ResponseModel):
    label: str = ""
    role: str = ""
    start_sec: float
    end_sec: float
    effects: list[PlanEffect] = []


class PlanStem(ResponseModel):
    stem: str
    path: str
    effects: list[PlanEffect] = []
    sections: list[PlanSection] = []


class SectionRole(ResponseModel):
    label: str = ""
    role: str = ""
    start_sec: float
    end_sec: float
    lufs: float | None = None


class CommandPlan(ResponseModel):
    mix_effects: list[PlanEffect] = []
    sections: list[PlanSection] = []
    stems: list[PlanStem] = []
    resolved_sections: list[SectionRole] = []


class ParsedCommand(ResponseModel):
    clause: str
    intent: str
    direction: int
    amount_db: float | None = None
    amount_pct: float | None = None
    scale: float = 1.0
    target_kind: str
    section: str = ""
    section_index: int | None = None
    stem: str = ""
    band_hz: float | None = None
    relative_to: str = ""
    start_sec: float | None = None
    end_sec: float | None = None
    effects: list[PlanEffect] = []
    explanation: str = ""


class CommandParseResponse(ResponseModel):
    text: str
    parser: str                       # "grammar" | "llm:<provider>" | "none"
    commands: list[ParsedCommand] = []
    plan: CommandPlan
    unparsed: list[str] = []
    unresolved: list[str] = []
    warnings: list[str] = []
    executable: bool = False
    plan_id: str = ""
    file_path: str = ""
    summary: str = ""


class VoiceCommandRequest(RequestModel):
    audio_path: str
    file_path: str = ""
    stem_paths: dict[str, str] = Field(default_factory=dict)
    allow_llm: bool = True
    model_size: str = "base"


class VoiceCommandResponse(CommandParseResponse):
    transcript: str = ""
    asr_model: str = ""


class CommandExecuteRequest(RequestModel):
    plan_id: str = Field(min_length=1, max_length=64)
    output_path: str = ""
    session_id: str = ""


class CommandExecuteResult(ResponseModel):
    status: str
    render_state: str = ""
    output_path: str = ""
    source_path: str = ""
    applied: list[str] = []
    summary: str = ""
    guardrail: RenderGuardrail | None = None
    quarantine_path: str = ""


class CommandJobStatusResponse(ResponseModel):
    status: JobStatus
    stage: str | None = None
    result: CommandExecuteResult | None = None
    error: str | None = None


# ---------------------------------------------------------------------------
# Parse
# ---------------------------------------------------------------------------
def _file_identity(path: str) -> tuple:
    try:
        st = os.stat(path)
        return (os.path.abspath(path), st.st_mtime_ns, st.st_size)
    except OSError:
        return (path, None, None)


def _store_plan(entry: dict) -> str:
    plan_id = uuid.uuid4().hex
    now = time.monotonic()
    with _plans_lock:
        for key in [k for k, v in _plans.items() if now - v["created"] > _PLAN_TTL_SEC]:
            _plans.pop(key, None)
        while len(_plans) >= _MAX_PLANS:
            _plans.pop(next(iter(_plans)))
        _plans[plan_id] = {**entry, "created": now}
    return plan_id


def _summary(commands: list[nl.Command], unresolved: list[str]) -> str:
    lines = [c.explanation for c in commands if c.explanation]
    if unresolved:
        lines.append("Not done: " + " ".join(unresolved))
    return "; ".join(lines) if lines else "Nothing to do."


def read_intents(text: str, allow_llm: bool = True) -> tuple[list[nl.Command], list[str], str, list[str]]:
    """Grammar parse plus the LLM fallback for clauses the grammar could not
    read. Returns (commands, unparsed, parser, warnings). Blocking only when
    the fallback runs (call it on the LLM pool)."""
    commands, unparsed = nl.parse_text(text)
    parser = "grammar" if commands else "none"
    warnings: list[str] = []
    if unparsed and allow_llm:
        llm_cmds, provider = nl.llm_fallback_commands(" ; ".join(unparsed))
        if llm_cmds:
            for cmd in llm_cmds:
                cmd.effects, cmd.explanation = nl.build_effects(cmd)
            commands.extend(llm_cmds)
            parser = f"llm:{provider}" if parser == "none" else f"grammar+llm:{provider}"
            warnings.append(f"Read by the {provider} model (structured intent only): "
                            + "; ".join(unparsed))
            unparsed = []
        elif not commands:
            warnings.append("No language model is available to read this phrasing.")
    return commands, unparsed, parser, warnings


def plan_commands(text: str, commands: list[nl.Command], unparsed: list[str], parser: str,
                  warnings: list[str], file_path: str = "",
                  stem_paths: dict[str, str] | None = None,
                  sections: list[dict] | None = None) -> dict:
    """Blocking (DSP): resolve section targets against the audio with the
    section analyzer, build the plan and store it for /command/execute."""
    warnings = list(warnings)
    roles: list[dict] | None = None
    lufs_of = None
    has_audio = bool(file_path) and os.path.isfile(file_path)
    needs_sections = any(c.target_kind == "section" or c.section for c in commands)
    needs_loudness = any(c.relative_to for c in commands)
    if has_audio and commands:
        if sections:
            import soundfile as sf
            info = sf.info(file_path)
            roles = nl.label_section_roles(sections, info.frames / float(info.samplerate))
            if needs_loudness:
                _roles, lufs_of = nl.analyze_section_roles(file_path)
        elif needs_sections or needs_loudness:
            roles, lufs_of = nl.analyze_section_roles(file_path)
        else:
            roles = []

    built = nl.build_plan(commands, roles=roles, stem_paths=stem_paths, lufs_of=lufs_of)
    plan = built["plan"]
    unresolved = built["unresolved"]
    warnings.extend(built["warnings"])
    executable = has_audio and not nl.plan_is_empty(plan)
    summary = _summary(commands, unresolved)
    plan_id = ""
    if executable:
        plan_id = _store_plan({"plan": plan, "file_path": file_path, "identity": _file_identity(file_path),
                               "text": text, "summary": summary})
    elif not has_audio and commands:
        warnings.append("No audio file was given, so nothing can be rendered yet.")
    return {
        "text": text,
        "parser": parser,
        "commands": [c.to_dict() for c in commands],
        "plan": plan,
        "unparsed": unparsed,
        "unresolved": unresolved,
        "warnings": warnings,
        "executable": executable,
        "plan_id": plan_id,
        "file_path": file_path,
        "summary": summary,
    }


def parse_command(text: str, file_path: str = "", stem_paths: dict[str, str] | None = None,
                  sections: list[dict] | None = None, allow_llm: bool = True) -> dict:
    """Both stages in one blocking call (voice commands, the MCP-free tests)."""
    commands, unparsed, parser, warnings = read_intents(text, allow_llm)
    return plan_commands(text, commands, unparsed, parser, warnings, file_path, stem_paths, sections)


async def _check_inputs(file_path: str, stem_paths: dict[str, str]) -> None:
    if file_path:
        require_file(file_path, "Audio file")
        await require_decode_budget(file_path)
    for name, path in stem_paths.items():
        if name not in ("vocals", "drums", "bass", "other"):
            invalid_input(f"Unknown stem '{name}' (vocals, drums, bass or other).")
        if path:
            require_file(path, f"{name} stem")
            await require_decode_budget(path)


@router.post("/command/parse", response_model=CommandParseResponse, response_model_exclude_unset=True)
async def command_parse(req: CommandParseRequest) -> dict:
    """Parse a free-text mix instruction into a render plan to preview.
    Nothing is rendered here; ``plan_id`` (when ``executable``) is what
    ``/command/execute`` renders after the user confirms."""
    await _check_inputs(req.file_path, req.stem_paths)
    sections = [s.model_dump() for s in req.sections]
    # A slow model (the fallback) runs on the LLM pool, the section analysis
    # on the DSP pool - neither blocks the other's workers.
    commands, unparsed, parser, warnings = await run_llm_blocking(read_intents, req.text, req.allow_llm)
    return await run_blocking(plan_commands, req.text, commands, unparsed, parser, warnings,
                              req.file_path, req.stem_paths, sections)


# ---------------------------------------------------------------------------
# Execute (job)
# ---------------------------------------------------------------------------
_command_jobs = JobStore(name="nl_command", max_active=2)
_command_futures: dict = {}


def _run_command_job(job_id: str, entry: dict, output_path: str, session_id: str, plan_id: str) -> None:
    if not job_begin(_command_jobs, job_id):
        return
    from khensu_services.copilot.router_render import (
        PostRenderGuardrailError, RENDER_STATE_BLOCKED, RENDER_STATE_VALIDATED, RenderBlocked,
        RenderCancelled, _log_applied_event, render_through_pipeline,
    )
    file_path = entry["file_path"]
    plan = entry["plan"]
    try:
        job_update(_command_jobs, job_id, stage="rendering")
        result, guardrail, _validation = render_through_pipeline(
            file_path, output_path,
            lambda quarantine: nl.render_plan(file_path, quarantine, plan),
            effects=nl.plan_effects(plan), graph=None,
            should_cancel=lambda: job_cancel_requested(_command_jobs, job_id))
        _log_applied_event("nl_command_applied", session_id, plan_id, output_path,
                           result.get("applied", []), plan)
        job_settle(_command_jobs, job_id, status=JobStatus.COMPLETED.value, stage="done", result={
            "status": "success", "render_state": RENDER_STATE_VALIDATED, "output_path": output_path,
            "source_path": file_path, "applied": result.get("applied", []),
            "summary": entry.get("summary", ""), "guardrail": guardrail,
        })
    except RenderCancelled:
        return
    except RenderBlocked as blocked:
        job_settle(_command_jobs, job_id, status=JobStatus.FAILED.value, error=str(blocked), result={
            "status": "blocked", "render_state": RENDER_STATE_BLOCKED, "source_path": file_path,
            "guardrail": {"pre_render": blocked.validation}})
    except PostRenderGuardrailError as exc:
        job_settle(_command_jobs, job_id, status=JobStatus.FAILED.value, error=str(exc), result={
            "status": "rejected", "render_state": exc.render_state, "source_path": file_path,
            "output_path": output_path, "quarantine_path": exc.quarantine_path or "",
            "guardrail": exc.measurements if isinstance(exc.measurements, dict) else None})
    except Exception as exc:
        _log.exception("NL command render failed for job %s", job_id)
        job_settle(_command_jobs, job_id, status=JobStatus.FAILED.value, error=str(exc))


@router.post("/command/execute", response_model=JobIdResponse, response_model_exclude_unset=True)
async def command_execute(req: CommandExecuteRequest) -> dict:
    """Render a previewed plan (by ``plan_id``) as a job."""
    with _plans_lock:
        entry = _plans.get(req.plan_id)
    if entry is None:
        not_found("Unknown or expired plan - parse the command again.")
    file_path = entry["file_path"]
    require_file(file_path, "Audio file")
    await require_decode_budget(file_path)
    if _file_identity(file_path) != entry["identity"]:
        conflict("The audio changed since the preview - parse the command again.")
    from khensu_services.common.output_paths import resolve_render_output
    stems = tuple(s["path"] for s in entry["plan"].get("stems") or [])
    output_path = resolve_render_output(req.output_path, file_path, f"cmd_{req.plan_id[:8]}",
                                        allowed_exts=(".wav",), extra_sources=stems)
    from khensu_services.copilot.render_validator import InvalidRenderParameter, validate_chain_parameters
    try:
        validate_chain_parameters(nl.plan_effects(entry["plan"]), None)
    except InvalidRenderParameter as exc:
        invalid_input(str(exc))
    job_id, _created = start_job(
        _command_jobs, long_job_executor, _run_command_job, entry, output_path, req.session_id,
        req.plan_id, payload={"status": JobStatus.PENDING.value, "stage": "queued", "result": None,
                              "error": None},
        dedup_key=f"{req.plan_id}|{output_path}", futures=_command_futures)
    return {"job_id": job_id}


@router.get("/command/execute/{job_id}", response_model=CommandJobStatusResponse,
            response_model_exclude_unset=True)
async def command_execute_status(job_id: str) -> dict:
    return get_job_or_404(_command_jobs, job_id)


@router.post("/command/execute/{job_id}/cancel", response_model=JobCancelResponse,
             response_model_exclude_unset=True)
async def command_execute_cancel(job_id: str) -> dict:
    return cancel_reply(_command_jobs, job_id, _command_futures)


# ---------------------------------------------------------------------------
# Voice (faster-whisper, extra [asr])
# ---------------------------------------------------------------------------
_ASR_SIZES = {"tiny", "base", "small"}


def _transcribe(audio_path: str, model_size: str) -> str:
    """Blocking: transcript text of a short spoken command. Raises
    HTTPException 503 when faster-whisper or its model is unavailable."""
    from khensu_services.analysis.lyric_analyzer import ASRNotAvailableError, _get_model
    try:
        model = _get_model(model_size)
    except ASRNotAvailableError as exc:
        unavailable(f"Voice commands need the ASR extra: {exc}", cause=exc)
    segments, _info = model.transcribe(
        audio_path, vad_filter=True, beam_size=1, language="en",
        initial_prompt="Mixing instruction: make the chorus louder, cut 300 Hz, widen the bridge, de-ess the vocal.")
    return " ".join(seg.text.strip() for seg in segments).strip()


def _voice_command(audio_path: str, model_size: str, file_path: str, stem_paths: dict[str, str],
                   allow_llm: bool) -> dict:
    transcript = _transcribe(audio_path, model_size)
    if not transcript:
        invalid_input("No speech was recognised in the recording.")
    out = parse_command(transcript[:_MAX_TEXT], file_path, stem_paths, None, allow_llm)
    out["transcript"] = transcript
    out["asr_model"] = f"faster-whisper:{model_size}"
    return out


@router.post("/command/voice", response_model=VoiceCommandResponse, response_model_exclude_unset=True)
async def command_voice(req: VoiceCommandRequest) -> dict:
    """Transcribe a spoken command (a recorded file) and parse it. 503 without
    the ``[asr]`` extra (faster-whisper)."""
    if req.model_size not in _ASR_SIZES:
        invalid_input(f"model_size must be one of {sorted(_ASR_SIZES)}")
    require_file(req.audio_path, "Voice recording")
    await require_decode_budget(req.audio_path)
    await _check_inputs(req.file_path, req.stem_paths)
    return await run_blocking(_voice_command, req.audio_path, req.model_size, req.file_path,
                              req.stem_paths, req.allow_llm)


