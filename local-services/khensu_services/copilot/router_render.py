"""Handles DSP chain application, rendering, delta soloing, parallel graph processing,
crossfeed A/B, and reference-matching workflows.
"""
from __future__ import annotations

import concurrent.futures
import logging
import os
from typing import Callable

from fastapi import APIRouter, HTTPException
from fastapi.responses import StreamingResponse
from pydantic import Field

from khensu_services.common.api_models import RequestModel, ResponseModel
from khensu_services.common.job_status import JobStatus

from khensu_services.copilot.dsp import apply_pedalboard_chain, apply_graph
from khensu_services.common.audio_limits import (
    AudioInputLimitError,
    AudioInputProbeError,
    validate_audio_decode_budget,
)
from khensu_services.common.errors import conflict, internal_error, invalid_input, not_found, too_large
from khensu_services.common.jobs import (
    JobStore, cancel_reply, get_job_or_404, job_begin, job_cancel_requested, job_events, job_settle,
    with_sse_prelude,
    request_fingerprint, start_job,
)
from khensu_services.common.executors import (
    dsp_executor as _executor,
    run_blocking,
)
from khensu_services.copilot.router_plugin_matching import EffectSpec
from khensu_services.copilot.response_models import (
    CopilotJobCancelResponse, JobStartResponse, OverrideGuardrailResponse,
    RenderGuardrail, RenderJobStatusResponse,
)

_log = logging.getLogger(__name__)
router = APIRouter()


async def _require_audio_decode_budget(*paths: str, allow_probe_errors: bool = False) -> None:
    """Reject unsafe render inputs before they occupy a DSP worker.

    Batch rendering keeps its documented per-file failure semantics for a
    corrupt/unreadable entry, while still rejecting every readable file that
    exceeds a concrete resource limit before the batch is admitted.
    """
    for path in paths:
        try:
            await run_blocking(validate_audio_decode_budget, path)
        except AudioInputLimitError as exc:
            too_large(str(exc), cause=exc)
        except AudioInputProbeError as exc:
            if not allow_probe_errors:
                invalid_input(f"Invalid audio file: {path}", cause=exc)

# Wall-clock deadline for the post-render guardrail (audit AI-01): a hanging
# check must fail closed rather than block a render forever. Opt-configurable.
_GUARDRAIL_TIMEOUT_SEC = float(os.environ.get("KHENSU_GUARDRAIL_TIMEOUT_SEC", "60"))


class PostRenderGuardrailError(RuntimeError):
    """The output exists but is not safe to present as an applied render.

    `kind` is a terminal classification (audit AI-01) distinct from a plain
    render failure: "missing" (the file the guardrail was supposed to check
    doesn't exist), "error" (the guardrail itself raised), or "rejected" (it
    ran and said no). `quarantine_path`/`measurements` are populated for
    "error"/"rejected" so a caller can offer an explicit expert override
    without re-rendering.
    """

    def __init__(self, message: str, *, kind: str, quarantine_path: str | None = None,
                 measurements: dict | None = None) -> None:
        super().__init__(message)
        self.kind = kind
        self.quarantine_path = quarantine_path
        self.measurements = measurements

    @property
    def render_state(self) -> str:
        return render_state_for_kind(self.kind)


# First-class terminal render states (audit AI-01), a single normalized
# vocabulary the desktop can switch on instead of re-deriving intent from
# `ok` plus the presence of quarantine fields:
#   validated   - guardrail passed; the render was promoted and is safe to use
#   rejected    - guardrail ran and refused the render (a real regression)
#   unvalidated - the guardrail couldn't run at all (missing output / it raised),
#                 so safety is unknown - treated as not-safe, same as rejected,
#                 but distinct so the UI can say "couldn't check" vs "failed the check"
RENDER_STATE_VALIDATED = "validated"
RENDER_STATE_REJECTED = "rejected"
RENDER_STATE_UNVALIDATED = "unvalidated"
# A pre-render refusal (genre abstention / stem-attribution mismatch / unsafe
# layout) - the render never ran at all, distinct from a post-render failure.
RENDER_STATE_BLOCKED = "blocked"


def render_state_for_kind(kind: str) -> str:
    """Maps a PostRenderGuardrailError.kind to its terminal render state."""
    if kind == "rejected":
        return RENDER_STATE_REJECTED
    # "missing" (no output to check) and "error" (the check itself blew up)
    # both mean safety is unknown.
    return RENDER_STATE_UNVALIDATED


def _quarantine_path_for(output_path: str) -> str:
    """Path a render is written to before its guardrail passes (audit AI-01):
    only a successful guardrail promotes this to the real requested
    output_path, via the atomic os.replace in _promote_quarantined_render().
    A missing/errored/rejected guardrail leaves the render here instead -
    never at the path the user/desktop actually asked for - available for an
    explicit, recorded expert override via POST /copilot/override_guardrail.
    """
    d = os.path.dirname(output_path) or "."
    return os.path.join(d, ".quarantine_" + os.path.basename(output_path))


def _same_path(a: str, b: str) -> bool:
    """Full-path equality after absolutizing/normalizing; case-insensitive on
    case-insensitive platforms (normcase is a no-op on POSIX)."""
    def _norm(p: str) -> str:
        return os.path.normcase(os.path.normpath(os.path.abspath(p)))
    return _norm(a) == _norm(b)


def _promote_quarantined_render(quarantine_path: str, output_path: str) -> None:
    """Atomically promotes a guardrail-passed (or expert-overridden) render
    from quarantine to the real requested path - the only way a render
    becomes the user-visible/auditionable output (audit AI-01)."""
    os.replace(quarantine_path, output_path)


def _discard_quarantined_render(quarantine_path: str) -> None:
    """Best-effort removal of a quarantined render nobody will promote (e.g.
    the job was cancelled while it rendered)."""
    try:
        os.remove(quarantine_path)
    except FileNotFoundError:
        pass
    except OSError:
        _log.warning("Could not remove quarantined render %s", quarantine_path, exc_info=True)


def _job_abandoned(job: dict) -> bool:
    """True once a job was cancelled/interrupted — its result must be discarded,
    never promoted or logged as applied."""
    return job.get("status") in (JobStatus.CANCELLED.value, JobStatus.CANCELLING.value, JobStatus.INTERRUPTED.value)


def _require_passing_guardrail(source_path: str, output_path: str) -> dict:
    """Run the mandatory post-render check and fail closed on every outcome.

    `output_path` here is the QUARANTINE path (see _quarantine_path_for) -
    callers must invoke this before promoting to the real requested path,
    logging an "applied" event, or reporting a completed/success result.
    """
    if not os.path.isfile(output_path):
        raise PostRenderGuardrailError("Render output is missing before guardrail validation", kind="missing")
    timeout = _GUARDRAIL_TIMEOUT_SEC
    try:
        from khensu_services.copilot.render_guardrails import guardrail_timeout_s, post_render_guardrail
        # Bounded so a HANGING guardrail (not just one that raises) fails closed
        # (audit AI-01): the check runs on its own thread with a wall-clock
        # deadline. A truly-hung blocking call can't be killed in Python, so on
        # timeout the request is failed and the render quarantined while the
        # orphaned thread is left to finish on its own (shutdown(wait=False)).
        _ex = concurrent.futures.ThreadPoolExecutor(max_workers=1)
        try:
            # The deadline grows with the track (a fixed 60 s failed every fix
            # of a long one); KHENSU_GUARDRAIL_TIMEOUT_SEC is the floor.
            timeout = guardrail_timeout_s(output_path, floor_s=_GUARDRAIL_TIMEOUT_SEC)
            guardrail = _ex.submit(post_render_guardrail, source_path, output_path).result(timeout=timeout)
        finally:
            _ex.shutdown(wait=False)
    except concurrent.futures.TimeoutError as exc:
        raise PostRenderGuardrailError(
            f"Post-render guardrail timed out after {timeout:.0f}s",
            kind="error", quarantine_path=output_path) from exc
    except Exception as exc:
        raise PostRenderGuardrailError("Post-render guardrail failed", kind="error",
                                       quarantine_path=output_path) from exc
    if not isinstance(guardrail, dict) or not guardrail.get("ok", False):
        raise PostRenderGuardrailError("Post-render guardrail rejected the output", kind="rejected",
                                       quarantine_path=output_path, measurements=guardrail)
    return guardrail


# ---------------------------------------------------------------------------
# The ONE render pipeline (audit S2 / CPL-01 / CPL-02 / SVC-10):
#     validate -> quarantine -> render -> guardrail -> promote
# Every render path - sync apply_chain (a flat chain or a graph), the apply_chain and
# section-chain jobs, every file of a batch, and the platform masters - goes
# through render_through_pipeline instead of a hand-copied variant. Batch and
# platform renders used to skip validator/quarantine/guardrail entirely and
# wrote straight to the final path.
# ---------------------------------------------------------------------------
class RenderBlocked(Exception):
    """The pre-render validator refused the request; nothing was rendered."""

    def __init__(self, validation: dict) -> None:
        super().__init__("Render blocked: " + "; ".join(validation.get("blocks") or []))
        self.validation = validation


class RenderCancelled(Exception):
    """The job was cancelled/interrupted mid-render; the quarantined output
    was discarded and must not be promoted, logged or reported."""


def render_through_pipeline(source_path: str, output_path: str,
                            render_fn: Callable[[str], dict], *,
                            effects: list[dict] | None = None, graph: dict | None = None,
                            validate: bool = True,
                            should_cancel: Callable[[], bool] | None = None,
                            guardrail_fn: Callable[[str, str], dict] | None = None,
                            ) -> tuple[dict, dict, dict | None]:
    """Run one render through the shared safety pipeline. Blocking.

    1. ``validate_render_request(source, effects, graph)`` - a block raises
       ``RenderBlocked``; a non-numeric parameter raises
       ``render_validator.InvalidRenderParameter`` (the HTTP layer's 422).
    2. ``render_fn(quarantine_path)`` renders into ``.quarantine_<name>``
       beside ``output_path``, never onto it.
    3. The post-render guardrail (``_require_passing_guardrail`` unless
       ``guardrail_fn`` is given) judges the quarantined file; a failure
       raises ``PostRenderGuardrailError`` and LEAVES the file in quarantine
       for the explicit expert override.
    4. Only a pass promotes it atomically onto ``output_path``.

    ``should_cancel`` is polled after the render and after the guardrail; a
    cancel discards the quarantined file and raises ``RenderCancelled``. Any
    other exception discards the quarantined file too (SVC-10) - only a
    guardrail verdict keeps one. Stale quarantine files in the output folder
    are swept first (retention window, render_retention).

    Returns ``(render_result, guardrail, validation)``.
    """
    validation: dict | None = None
    if validate:
        from khensu_services.copilot.render_validator import validate_render_request
        validation = validate_render_request(source_path, effects, graph)
        if not validation.get("ok", False):
            raise RenderBlocked(validation)

    out_dir = os.path.dirname(output_path)
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)
    from khensu_services.copilot.render_retention import sweep_stale_quarantine
    sweep_stale_quarantine(out_dir or ".")

    quarantine_path = _quarantine_path_for(output_path)
    try:
        result = render_fn(quarantine_path)
        if should_cancel is not None and should_cancel():
            _discard_quarantined_render(quarantine_path)
            raise RenderCancelled()
        if guardrail_fn is None:
            guardrail = _require_passing_guardrail(source_path, quarantine_path)
        else:
            guardrail = guardrail_fn(source_path, quarantine_path)
        if should_cancel is not None and should_cancel():
            _discard_quarantined_render(quarantine_path)
            raise RenderCancelled()
        if validation and validation.get("warnings"):
            guardrail.setdefault("pre_render", validation)
        _promote_quarantined_render(quarantine_path, output_path)
    except (PostRenderGuardrailError, RenderCancelled):
        raise
    except BaseException:
        # A crashed render/guardrail/promotion must not strand a quarantine
        # file nobody can override (SVC-10).
        _discard_quarantined_render(quarantine_path)
        raise
    return result if isinstance(result, dict) else {}, guardrail, validation


def _effect_dicts(effects) -> list[dict]:
    return [{"effect": e.effect, "parameters": e.parameters} for e in effects]


def _graph_dict(graph: "RenderGraph | None") -> dict | None:
    """The typed request graph as the plain dict dsp.apply_graph takes."""
    if graph is None:
        return None
    out: dict = {
        "branches": [
            {"effects": _effect_dicts(b.effects), "gain_db": b.gain_db, "mix": b.mix,
             "band_lo_hz": b.band_lo_hz, "band_hi_hz": b.band_hi_hz}
            for b in graph.branches
        ],
        "post": _effect_dicts(graph.post),
    }
    if graph.effects:
        out["effects"] = _effect_dicts(graph.effects)
    if not out["branches"] and not out["post"] and not graph.effects:
        return None
    return out


def _file_identity(*paths: str) -> list:
    """(path, mtime_ns, size) for each existing path - dedup keys include it so
    a file re-rendered in place mid-job never joins the stale job (SVC-07)."""
    ident = []
    for p in paths:
        try:
            st = os.stat(p)
            ident.append([os.path.abspath(p), st.st_mtime_ns, st.st_size])
        except OSError:
            ident.append([p, None, None])
    return ident


def _require_valid_parameters(effects: list[dict] | None, graph: dict | None) -> None:
    """422 for a chain parameter the validator can't read as a number (CPL-09)."""
    from khensu_services.copilot.render_validator import (
        InvalidRenderParameter, validate_chain_parameters,
    )
    try:
        validate_chain_parameters(effects, graph)
    except InvalidRenderParameter as exc:
        invalid_input(str(exc))


def _cancel_reply(store: JobStore, job_id: str, futures: dict | None = None) -> dict:
    """SVC-14: ``ok`` only for a known job, plus its resulting status
    (``common.jobs.cancel_reply``); with ``futures`` a still-queued job is
    cancelled outright and frees its admission slot."""
    return cancel_reply(store, job_id, futures)


# llm_config.decision_hash emits a 16-char hex digest; allow up to 64 hex
# chars (a full sha256) so a future widening isn't a breaking change, but
# nothing else - the value is copied verbatim into provenance payloads.
_DECISION_HASH_MAX_LEN = 64
_DECISION_HASH_PATTERN = r"^[0-9a-fA-F]*$"


def _decision_hash_field():
    return Field(default="", max_length=_DECISION_HASH_MAX_LEN, pattern=_DECISION_HASH_PATTERN)


def _genre_action_block(req: object) -> str | None:
    """Fail closed when a chain claims genre-relative intent without proof."""
    if bool(getattr(req, "genre_relative", False)) and not bool(getattr(req, "genre_action_allowed", False)):
        return (getattr(req, "abstained_reason", "")
                or "Genre-relative action was abstained: confirm a candidate or provide a reference track first.")
    return None


class MixerChainRequest(RequestModel):
    target_channel: str
    genre: str
    energy: str
    mood: str
    bpm: float
    style_description: str
    enabled_ecosystems: list[str]
    vocal_strategy: str | None = None
    vocal_macros: dict[str, float] | None = None
    # UserPreferenceModel's learned accept/reject offsets (see
    # TargetOverrideEditor/AnalysisController on the desktop side) — feeds
    # the same "copilot that learns" signal into chain generation, not just
    # genre-target nudging. 0.0 = no learned preference yet.
    preference_lufs_offset: float = 0.0
    preference_lra_offset: float = 0.0
    preference_stereo_width_offset: float = 0.0
    preference_low_end_weight_offset: float = 0.0


class GraphBranch(RequestModel):
    effects: list[EffectSpec] = Field(default_factory=list, max_length=64)
    gain_db: float = 0.0
    mix: float = 1.0
    band_lo_hz: float = 0.0
    band_hi_hz: float = 0.0


class RenderGraph(RequestModel):
    """A processing graph sent inside apply_chain / batch requests: the same
    typed branches/post a graph render takes (audit §6.3 - ``graph: dict``
    was an untyped side door straight into the renderer). ``effects`` is the
    flat single-branch form dsp.apply_graph also accepts."""
    branches: list[GraphBranch] = Field(default_factory=list, max_length=32)
    post: list[EffectSpec] = Field(default_factory=list, max_length=64)
    effects: list[EffectSpec] = Field(default_factory=list, max_length=64)


class ApplyChainRequest(RequestModel):
    file_path: str
    output_path: str
    effects: list[EffectSpec] = Field(default_factory=list, max_length=64)
    graph: RenderGraph | None = None
    # False (default) preserves the chain's own gain staging (clip-guard only);
    # True restores the legacy peak-normalize-to--1.0 dBFS/dBTP behaviour.
    normalize: bool = False
    # Optional provenance metadata — when session_id is set, a chain_applied
    # event is logged so the "what changed and why" timeline can show it.
    session_id: str = ""
    source_recommendation_id: str = ""
    # Decision fingerprint from the ChatResponse/MixerChainResponse that
    # produced this chain (see llm_config.decision_hash) — when the caller
    # forwards it, the applied-render event ties back to exactly which
    # model/prompt/heuristic-inputs decided it, for reproducibility/audit.
    decision_hash: str = _decision_hash_field()
    # Genre-relative chains must carry an explicit confirmation/reference
    # decision. Defaults fail closed for LLM and UI callers that omit it.
    genre_relative: bool = False
    genre_action_allowed: bool = False
    abstained_reason: str = ""
    target_stem: str = "mix"
    stem_paths: dict[str, str] = Field(default_factory=dict)
    # ENH-6: the stem (a key of stem_paths, e.g. "vocals") whose signal keys
    # the chain's DynamicEQ stages with use_key 1 - the native renderer's
    # ChainRender::keyStem. Unknown or missing -> they key on their own input
    # and the response carries a warning; never an error.
    key_stem: str = Field(default="", max_length=64)


class ApplyChainResponse(ResponseModel):
    status: str
    output_path: str
    # First-class terminal render state (audit AI-01): "validated" on a
    # promoted render, "blocked" for a pre-render refusal (genre/attribution/
    # unsafe-layout), else the guardrail failure state. Lets the desktop
    # switch on one field instead of re-deriving intent from status+guardrail.
    render_state: str = RENDER_STATE_VALIDATED
    applied_effects: list[str] = []
    skipped_effects: list[str] = []
    # Diagnostic report per TapeSaturation stage in the chain — how much
    # near-Nyquist energy the nonlinearity added, where un-oversampled
    # aliasing would fold back (see dsp.analyze_saturation_aliasing).
    harmonic_reports: list[dict] = []
    # Per-stage {label, in/out_rms_db, in/out_peak_db} levels from the DSP
    # chain - the desktop's gain-staging waterfall reads these.
    stage_levels: list[dict] = []
    # Post-render safety guardrail (2026-07-15 safety audit P0): matched-level
    # before/after comparison. `guardrail.ok == False` means a hard regression
    # (true-peak/clipping/mono-fold) the UI must not silently present as an
    # improvement. None only if the source file was unavailable to compare.
    guardrail: RenderGuardrail | None = None
    # Non-fatal render notes, e.g. a key stem that could not be used (ENH-6).
    warnings: list[str] | None = None


def resolve_key_stem(key_stem: str, stem_paths: dict[str, str] | None) -> tuple[str | None, list[str]]:
    """ENH-6: the key signal's file for ``key_stem``, looked up in the
    request's ``stem_paths``. An unknown stem or a missing file is not an
    error: ``(None, [warning])``, and the keyed stages key on their own input
    (as the native renderer does when ``getStemFile`` finds nothing)."""
    name = (key_stem or "").strip()
    if not name:
        return None, []
    paths = stem_paths or {}
    path = paths.get(name)
    if path is None:
        lowered = {k.strip().lower(): v for k, v in paths.items()}
        path = lowered.get(name.lower())
    if not path:
        return None, [f"Key stem '{name}' is not among the declared stems; "
                      "the dynamic EQ keyed on its own input."]
    if not os.path.isfile(path):
        return None, [f"Key stem '{name}' file was not found; "
                      "the dynamic EQ keyed on its own input."]
    return path, []


async def resolve_key_stem_checked(key_stem: str, stem_paths: dict[str, str] | None
                                   ) -> tuple[str | None, list[str]]:
    """resolve_key_stem plus the decode budget: a key over the budget is a
    413 like any other render input; an unreadable one falls back at render
    time with a warning."""
    key_path, warnings = resolve_key_stem(key_stem, stem_paths)
    if key_path:
        await _require_audio_decode_budget(key_path, allow_probe_errors=True)
    return key_path, warnings


def warnings_kw(warnings: list[str]) -> dict:
    """``{"warnings": [...]}`` only when there are any, so the field stays
    off the wire otherwise (the routes exclude unset fields)."""
    return {"warnings": list(warnings)} if warnings else {}


def _chain_render_fn(file_path: str, effects: list[dict], graph: dict | None,
                     normalize: bool, key_path: str | None = None) -> Callable[[str], dict]:
    """The render step for a flat chain or a graph, as a pipeline render_fn.
    Looks the renderers up at call time so tests can patch them. ``key_path``
    (ENH-6) is only passed when set, so patched renderers without it work."""
    kw = {"key_path": key_path} if key_path else {}
    if graph:
        return lambda quarantine: apply_graph(file_path, quarantine, graph, normalize=normalize, **kw)
    return lambda quarantine: apply_pedalboard_chain(file_path, quarantine, effects, normalize=normalize,
                                                     **kw)


def _guardrail_conflict(exc: PostRenderGuardrailError, source_path: str, output_path: str) -> HTTPException:
    return HTTPException(status_code=409, detail={
        "message": str(exc), "kind": exc.kind, "render_state": exc.render_state,
        "quarantine_path": exc.quarantine_path, "measurements": exc.measurements,
        "source_path": source_path, "output_path": output_path,
    })


@router.post("/apply_chain", response_model=ApplyChainResponse, response_model_exclude_unset=True)
async def apply_chain(req: ApplyChainRequest) -> ApplyChainResponse:
    """Applies a suggested Pedalboard DSP chain to an audio file and saves the output."""
    blocked_reason = _genre_action_block(req)
    if blocked_reason:
        return ApplyChainResponse(status="blocked", output_path="", render_state=RENDER_STATE_BLOCKED,
                                  guardrail={"pre_render": {"ok": False, "blocks": [blocked_reason]}})
    from khensu_services.copilot.render_validator import validate_stem_attribution
    attribution = validate_stem_attribution(req.file_path, req.target_stem, req.stem_paths)
    if not attribution["ok"]:
        return ApplyChainResponse(status="blocked", output_path="", render_state=RENDER_STATE_BLOCKED,
                                  guardrail={"pre_render": attribution})
    if not os.path.isfile(req.file_path):
        not_found(f"File not found: {req.file_path}")
    await _require_audio_decode_budget(req.file_path)
    key_path, key_warnings = await resolve_key_stem_checked(req.key_stem, req.stem_paths)

    # Containment: the rendered WAV may only be written under the user's home,
    # temp, or the source file's directory (parity with /separate/start).
    from khensu_services.common.output_paths import validate_output_path, reject_output_alias
    validate_output_path(req.output_path, req.file_path)
    reject_output_alias(req.output_path, req.file_path, *req.stem_paths.values())

    graph = _graph_dict(req.graph)
    effect_list = _effect_dicts(req.effects)
    _require_valid_parameters(None if graph else effect_list, graph)

    try:
        # validate (blocking pre-render safety validator, safety audit P1)
        # -> quarantine -> render -> guardrail -> promote (audit AI-01 / S2).
        result, guardrail, _validation = await run_blocking(
            render_through_pipeline, req.file_path, req.output_path,
            _chain_render_fn(req.file_path, effect_list, graph, req.normalize, key_path),
            effects=None if graph else effect_list, graph=graph)
    except RenderBlocked as blocked:
        return ApplyChainResponse(status="blocked", output_path="", render_state=RENDER_STATE_BLOCKED,
                                  guardrail={"pre_render": blocked.validation})
    except PostRenderGuardrailError as exc:
        _log.warning("Render rejected by post-render guardrail for %s: %s", req.output_path, exc)
        raise _guardrail_conflict(exc, req.file_path, req.output_path) from exc
    except Exception as exc:
        from khensu_services.copilot.render_validator import InvalidRenderParameter
        if isinstance(exc, InvalidRenderParameter):
            invalid_input(str(exc))
        _log.exception("Failed to apply chain to %s", req.file_path)
        internal_error()

    _log_applied_event("chain_applied", req.session_id, req.source_recommendation_id,
                       req.output_path, result.get("applied", []), graph if graph else effect_list,
                       req.decision_hash)
    return ApplyChainResponse(
        status="success",
        output_path=req.output_path,
        # Explicit: the route excludes unset fields, so a defaulted
        # render_state would vanish from the wire (SVC-05).
        render_state=RENDER_STATE_VALIDATED,
        applied_effects=result.get("applied", []),
        skipped_effects=result.get("skipped", []),
        harmonic_reports=result.get("harmonic_reports", []),
        stage_levels=result.get("stage_levels", []),
        guardrail=guardrail,
        **warnings_kw(key_warnings + list(result.get("warnings") or [])),
    )


# ---------------------------------------------------------------------------
# Job-queue variant of apply_chain — addresses the audit finding that render
# operations had no cancellation, unlike /analyze/start and /separate/start
# which already have the start/status/cancel pattern below. A render can run
# long (large files, 4x-oversampled TapeSaturation), so the desktop UI needs
# a way to show progress and let the user back out without waiting for it.
# Cancellation is cooperative, same caveat as the other two job queues: the
# render already in flight isn't interrupted mid-call, but its result is
# discarded once the job is marked cancelled.
# ---------------------------------------------------------------------------
_apply_jobs = JobStore(name="apply_chain", max_active=6)  # bounded (F2) + admission cap (ARCH-01)
_apply_futures: dict = {}


def _rejected_job_result(exc: PostRenderGuardrailError, source_path: str, output_path: str) -> dict:
    return {
        "status": "rejected",
        "guardrail_status": exc.kind, "render_state": exc.render_state,  # terminal non-success
        "quarantine_path": exc.quarantine_path,
        "measurements": exc.measurements,
        "source_path": source_path,
        "output_path": output_path,
    }


def _run_apply_job(job_id: str, req: ApplyChainRequest) -> None:
    if not job_begin(_apply_jobs, job_id):
        return
    try:
        from khensu_services.copilot.render_validator import validate_stem_attribution
        attribution = validate_stem_attribution(req.file_path, req.target_stem, req.stem_paths)
        if not attribution["ok"]:
            job_settle(_apply_jobs, job_id,
                       status=JobStatus.FAILED.value, error="Render blocked: " + "; ".join(attribution["blocks"]),
                       result={"status": "blocked", "render_state": RENDER_STATE_BLOCKED,
                               "guardrail": {"pre_render": attribution}})
            return

        graph = _graph_dict(req.graph)
        effect_list = _effect_dicts(req.effects)
        key_path, key_warnings = resolve_key_stem(req.key_stem, req.stem_paths)
        result, guardrail, _validation = render_through_pipeline(
            req.file_path, req.output_path,
            _chain_render_fn(req.file_path, effect_list, graph, req.normalize, key_path),
            effects=None if graph else effect_list, graph=graph,
            should_cancel=lambda: job_cancel_requested(_apply_jobs, job_id))
        _log_applied_event("chain_applied", req.session_id, req.source_recommendation_id,
                           req.output_path, result.get("applied", []),
                           graph if graph else effect_list, req.decision_hash)

        job_result = {
            "status": "success",
            "render_state": RENDER_STATE_VALIDATED,
            "output_path": req.output_path,
            "applied_effects": result.get("applied", []),
            "skipped_effects": result.get("skipped", []),
            "harmonic_reports": result.get("harmonic_reports", []),
            "stage_levels": result.get("stage_levels", []),
            "guardrail": guardrail,
            **warnings_kw(key_warnings + list(result.get("warnings") or [])),
        }
        # Echoed so a render can be traced to its model decision even when no
        # session_id was sent (the provenance event is session-scoped).
        if req.decision_hash:
            job_result["decision_hash"] = req.decision_hash
        job_settle(_apply_jobs, job_id, status=JobStatus.COMPLETED.value, result=job_result)
    except RenderCancelled:
        # Cancelled while rendering: discarded, never promoted or logged applied.
        return
    except RenderBlocked as blocked:
        job_settle(_apply_jobs, job_id, status=JobStatus.FAILED.value, error=str(blocked),
                   result={"status": "blocked", "render_state": RENDER_STATE_BLOCKED,
                           "guardrail": {"pre_render": blocked.validation}})
    except PostRenderGuardrailError as exc:
        _log.warning("Render rejected by post-render guardrail for job %s: %s", job_id, exc)
        job_settle(_apply_jobs, job_id, status=JobStatus.FAILED.value, error=str(exc),
                   result=_rejected_job_result(exc, req.file_path, req.output_path))
    except Exception as exc:
        _log.exception("Background apply_chain failed for job %s", job_id)
        job_settle(_apply_jobs, job_id, status=JobStatus.FAILED.value, error=str(exc))


@router.post("/apply_chain/start", response_model=JobStartResponse, response_model_exclude_unset=True)
async def apply_chain_start(req: ApplyChainRequest) -> dict:
    blocked_reason = _genre_action_block(req)
    if blocked_reason:
        conflict(blocked_reason)
    if not os.path.isfile(req.file_path):
        not_found(f"File not found: {req.file_path}")
    await _require_audio_decode_budget(req.file_path)

    from khensu_services.copilot.render_validator import validate_stem_attribution
    attribution = validate_stem_attribution(req.file_path, req.target_stem, req.stem_paths)
    if not attribution["ok"]:
        conflict("; ".join(attribution["blocks"]))
    await resolve_key_stem_checked(req.key_stem, req.stem_paths)

    from khensu_services.common.output_paths import validate_output_path, reject_output_alias
    validate_output_path(req.output_path, req.file_path)
    reject_output_alias(req.output_path, req.file_path, *req.stem_paths.values())
    graph = _graph_dict(req.graph)
    _require_valid_parameters(None if graph else _effect_dicts(req.effects), graph)

    # Dedup (audit ARCH-01): a double-click or a client retry on the exact
    # same (source, destination) joins the already-running job instead of
    # starting a redundant second render.
    # The key includes a hash of every request parameter (chain/graph/
    # normalize/stems/...) and the source's mtime+size (SVC-07), so a
    # *different* render to the same destination - or the same render of a
    # file re-rendered in place - is never joined onto a stale job.
    dedup_key = (f"{req.file_path}|{req.output_path}|"
                 f"{request_fingerprint(req, _file_identity(req.file_path, *req.stem_paths.values()))}")
    job_id, _created = start_job(
        _apply_jobs, _executor, _run_apply_job, req,
        payload={"status": JobStatus.PENDING.value, "result": None, "error": None}, dedup_key=dedup_key,
        futures=_apply_futures)
    return {"job_id": job_id}


@router.get("/apply_chain/status/{job_id}", response_model=RenderJobStatusResponse, response_model_exclude_unset=True)
async def apply_chain_status(job_id: str) -> dict:
    return get_job_or_404(_apply_jobs, job_id)


@router.get("/apply_chain/events/{job_id}", response_class=StreamingResponse)
async def apply_chain_events(job_id: str) -> StreamingResponse:
    """PERF-6: the render's status as server-sent events (one per change,
    ending at a terminal state) instead of the client polling the status
    route every 750 ms. Same payload as /apply_chain/status."""
    get_job_or_404(_apply_jobs, job_id)
    return StreamingResponse(with_sse_prelude(job_events(_apply_jobs, job_id, RenderJobStatusResponse)),
                             media_type="text/event-stream", headers={"Cache-Control": "no-cache"})


@router.post("/apply_chain/{job_id}/cancel", response_model=CopilotJobCancelResponse, response_model_exclude_unset=True)
async def apply_chain_cancel(job_id: str) -> dict:
    return _cancel_reply(_apply_jobs, job_id, _apply_futures)


def _log_applied_event(event_type: str, session_id: str, rec_id: str,
                       output_path: str, applied: list[str], chain,
                       decision_hash_val: str = "") -> None:
    """Best-effort provenance log — never fail the render if logging fails."""
    if not session_id:
        return
    try:
        from khensu_services.events import db as events_db
        import os as _os
        payload = {"output_path": output_path, "applied": applied, "chain": chain}
        if decision_hash_val:
            payload["decision_hash"] = decision_hash_val
        events_db.append_event(
            session_id=session_id,
            event_type=event_type,
            summary=f"Applied {', '.join(applied)} -> {_os.path.basename(output_path)}",
            recommendation_id=rec_id,
            payload=payload,
        )
    except Exception:
        _log.warning("provenance event log failed", exc_info=True)


class OverrideGuardrailRequest(RequestModel):
    source_path: str
    quarantine_path: str
    output_path: str
    session_id: str = ""
    source_recommendation_id: str = ""
    # See ApplyChainRequest.decision_hash - an overridden render keeps its trace.
    decision_hash: str = _decision_hash_field()
    # Required (min_length=1): there is no way to override silently - the
    # user must type an explicit justification (audit AI-01's "expert override").
    expert_note: str = Field(min_length=1, max_length=2000)


@router.post("/override_guardrail", response_model=OverrideGuardrailResponse, response_model_exclude_unset=True)
async def override_guardrail(req: OverrideGuardrailRequest) -> dict:
    """Promotes a guardrail-quarantined render to its intended output path
    anyway, on explicit expert authority (audit AI-01).

    Re-measures the guardrail against the quarantined file rather than
    trusting client-supplied numbers, so the recorded measurements are
    authoritative - the client only supplies *why* it's being overridden, not
    *what* the measurements were. Logs a distinct 'guardrail_override'
    security event plus a provenance event carrying the note and the
    re-measured numbers, so an override is always attributable after the
    fact, never silent.
    """
    if not os.path.isfile(req.quarantine_path):
        not_found("Quarantined render not found (already promoted, discarded, or the path is wrong).")
    await _require_audio_decode_budget(req.source_path, req.quarantine_path)
    # Containment: only ever promote a file this service itself quarantined
    # for this exact output_path - not an arbitrary path a caller supplies.
    # The FULL normalized path must match (a basename-only check let a caller
    # promote a same-named file from any other directory).
    if not _same_path(req.quarantine_path, _quarantine_path_for(req.output_path)):
        invalid_input("quarantine_path does not correspond to output_path.")
    from khensu_services.common.output_paths import validate_output_path, reject_output_alias
    validate_output_path(req.output_path, req.source_path)
    # Parity with the render endpoints: never promote onto the source itself.
    reject_output_alias(req.output_path, req.source_path)

    try:
        from khensu_services.copilot.render_guardrails import post_render_guardrail
        measurements = await run_blocking(post_render_guardrail, req.source_path, req.quarantine_path)
    except Exception:
        _log.exception("Could not re-measure the guardrail for override of %s", req.quarantine_path)
        internal_error()

    await run_blocking(_promote_quarantined_render, req.quarantine_path, req.output_path)

    from khensu_services.common.security_log import record_event, KIND_GUARDRAIL_OVERRIDE
    record_event(KIND_GUARDRAIL_OVERRIDE,
                f"Promoted {req.output_path} despite guardrail rejection - expert_note: {req.expert_note[:200]!r}")

    if req.session_id:
        try:
            from khensu_services.events import db as events_db
            payload = {"output_path": req.output_path, "expert_note": req.expert_note, "measurements": measurements}
            if req.decision_hash:
                payload["decision_hash"] = req.decision_hash
            events_db.append_event(
                session_id=req.session_id,
                event_type="guardrail_override",
                summary=f"Expert override: promoted despite guardrail measurements ({req.expert_note[:120]})",
                recommendation_id=req.source_recommendation_id,
                payload=payload,
            )
        except Exception:
            _log.warning("provenance event log failed for guardrail override", exc_info=True)

    return {"status": "success", "output_path": req.output_path, "measurements": measurements}


