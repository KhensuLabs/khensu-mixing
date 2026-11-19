# local-services/khensu_services/analysis/router_full_intake.py
"""Intake orchestrator (spec §1): one call runs the mix-level analysis stages
as a staged pass, returns them as a single bundle with per-stage status, and
caches the bundle per song (mtime+size). Panels can still call their own
endpoints; this gives a single cached snapshot for reopening a song instantly
and a legible stage list for the progress HUD.

Stem-dependent stages (arrangement, depth, stem taxonomy, masking) are NOT
run here — they need Demucs output and are triggered separately once
separation lands. This orchestrator is the fast/standard mix-level pass.
"""
from __future__ import annotations
import logging
import os
import threading
from concurrent.futures import Future, ThreadPoolExecutor
from typing import Any, Callable

from fastapi import APIRouter

from khensu_services.analysis.analyzer_responses import (
    GenreContextResponse, QuickStatsResponse, SignalIntegrityResponse,
    StemTaxonomyResponse,
)
from khensu_services.analysis.artifacts.api_models import ArtifactScanResponse
from khensu_services.analysis.models import (
    ArrangementResponse, DepthResponse, FullIntakeRequest, MaskingResponse, PanBalanceResponse,
    SourceTriageResponse,
)
from khensu_services.common.errors import internal_error, not_found, require_decode_budget, require_file
from khensu_services.common.jobs import (
    JobStore, cancel_job, get_job_or_404, job_begin, job_cancel_requested,
    job_settle, job_update, start_job,
)
from khensu_services.common.api_models import ResponseModel
from khensu_services.common.job_status import JobStatus
from khensu_services.common.lru import BoundedCache
from khensu_services.common import analysis_cache as _acache
from khensu_services.common.executors import long_job_executor, run_blocking, run_long_blocking

_log = logging.getLogger(__name__)
router = APIRouter()


async def _require_audio_decode_budget(*paths: str) -> None:
    """Check every existing intake input before queuing full decodes."""
    await require_decode_budget(*paths, skip_missing=True)


def _intake_audio_paths(req: FullIntakeRequest) -> tuple[str, ...]:
    return (req.file_path, req.reference_path, *req.stems.values())

# Bounded so a long session can't grow service memory without limit (audit F2).
# Intake jobs track lifecycle under "state" (not "status") and share the same
# terminal set as every other endpoint.
_intake_cache: BoundedCache[tuple, dict[str, Any]] = BoundedCache(max_entries=32)
_intake_jobs: JobStore = JobStore(name="full_intake", status_key="state", max_active=3)

class _IntakeCancelled(Exception):
    """Cooperative cancellation between analyzer stages."""


# AID-15: the five independent stages used to run on a fresh
# ThreadPoolExecutor(4) per intake job, on top of the dsp_executor worker the
# job already held - 3 admitted jobs meant up to 12 heavy decode/STFT threads
# (~680 MiB each on a 3-minute track). One process-wide, bounded pool now
# serves every job's stage fan-out, so the heavy-thread total stays at
# _STAGE_WORKERS however many intakes are admitted. (Submitting the stages to
# dsp_executor itself could deadlock: the job holds a dsp worker while it waits
# on its stages.)
_STAGE_WORKERS = 4
_stage_pool: ThreadPoolExecutor | None = None
_stage_pool_lock = threading.Lock()


def _intake_stage_pool() -> ThreadPoolExecutor:
    global _stage_pool
    with _stage_pool_lock:
        if _stage_pool is None:
            _stage_pool = ThreadPoolExecutor(max_workers=_STAGE_WORKERS,
                                             thread_name_prefix="intake-stage")
        return _stage_pool


def _cache_key(file_path: str, stated_genre_id: str, reference_path: str,
               use_genre_targets: bool, confirm_detected_genre: bool,
               selection_mode: str, user_metadata: dict[str, str], stems: dict[str, str]) -> tuple:
    st = os.stat(file_path)
    ref_state = ""
    if reference_path and os.path.isfile(reference_path):
        ref_st = os.stat(reference_path)
        ref_state = f"{reference_path}:{ref_st.st_mtime_ns}:{ref_st.st_size}"
    return (file_path, st.st_mtime_ns, st.st_size, stated_genre_id, ref_state,
            use_genre_targets, confirm_detected_genre, selection_mode,
            tuple(sorted(user_metadata.items())),
            tuple(sorted((name, path, os.stat(path).st_mtime_ns, os.stat(path).st_size)
                         for name, path in stems.items() if os.path.isfile(path))))


def _run_intake(file_path: str, stated_genre_id: str, reference_path: str,
                use_genre_targets: bool, confirm_detected_genre: bool,
                selection_mode: str, user_metadata: dict[str, str], stems: dict[str, str],
                progress: Callable[[str, str], bool | None] | None = None) -> dict[str, Any]:
    # (stage name, callable) — order is the pipeline order the HUD shows.
    from khensu_services.analysis.quick_stats import analyze_quick_stats
    from khensu_services.analysis.signal_integrity import analyze_signal_integrity
    from khensu_services.analysis.source_triage import triage_cache_params, triage_source
    from khensu_services.analysis.pan_balance import analyze_pan_balance
    from khensu_services.analysis.artifacts import analyze_artifacts
    from khensu_services.analysis.genre_context import build_genre_context
    from khensu_services.analysis.context import AnalysisContext

    # One shared decode/transform cache for every stage that reads the mix
    # (pipeline enhancement #1): the mix-level stages below each used to
    # re-decode the same file. Golden-safe — the context reproduces each
    # analyzer's own librosa.load call, just once.
    actx = AnalysisContext(file_path)

    required_stems = ("vocals", "drums", "bass", "other")
    has_stems = all(os.path.isfile(stems.get(name, "")) for name in required_stems)

    def _stem_context() -> dict[str, Any]:
        if not has_stems:
            return {"deferred": True, "reason": "Waiting for four verified separated stems."}
        # AID-07: HPSS fallback "stems" share Demucs' file names but "vocals"
        # is the whole harmonic mix - arrangement, depth, taxonomy shares and
        # masking computed on them are meaningless. Refuse, and say why.
        from khensu_services.analysis.separator import (
            METHOD_HPSS, hpss_stems_note, stems_separation_method)
        if stems_separation_method({n: stems[n] for n in required_stems}) == METHOD_HPSS:
            return {"deferred": True,
                    "reason": hpss_stems_note() + " Stem analyses need model-separated "
                              "(Demucs) stems."}
        from khensu_services.analysis.arrangement_analyzer import analyze_arrangement
        from khensu_services.analysis.depth_mapper import analyze_depth
        from khensu_services.analysis.stem_taxonomy import analyze_stem_taxonomy
        from khensu_services.analysis.masking import compute_stem_masking
        genre_id = results.get("musical_context", {}).get("selected_genre_id", "")

        # The four stem analyzers each take seconds; check cancellation (and
        # report finer sub-stage progress) between them so a cancel during stem
        # processing is honoured promptly instead of only after the whole stage.
        def _check(sub: str) -> None:
            if progress and progress("stem_context", sub) is False:
                raise _IntakeCancelled()

        out: dict[str, Any] = {}
        _check("arrangement")
        out["arrangement"] = analyze_arrangement(*(stems[name] for name in required_stems))
        _check("depth")
        out["depth"] = analyze_depth(stems)
        _check("stem_taxonomy")
        out["stem_taxonomy"] = analyze_stem_taxonomy(
            *(stems[name] for name in required_stems),
            genre={"id": genre_id} if genre_id else None)
        _check("masking")
        out["masking"] = compute_stem_masking(stems)
        return out

    def _musical_context() -> dict[str, Any]:
        # Shared, persistent quick pass (CE-PLT-2 / PERF-2): /analyze quick
        # and /analyze/sections reuse this result instead of re-running it.
        from khensu_services.analysis.router_audio import cached_analyze_quick
        quick = cached_analyze_quick(file_path, ctx=actx)
        context = build_genre_context(
            file_path, quick,
            stated_genre_id=stated_genre_id,
            reference_path=reference_path,
            use_genre_targets=use_genre_targets,
            confirm_detected_genre=confirm_detected_genre,
            selection_mode=selection_mode,
            user_metadata=user_metadata,
            source_triage=results.get("source_triage"),
            integrity=results.get("signal_integrity"),
        )
        # Preserve the musical primitives required by downstream callers; the
        # context is an additive replacement for the former disconnected
        # quick/extended genre calls.
        context["quick_analysis"] = quick
        return context

    stages: list[tuple[str, Callable[[], Any]]] = [
        # These three go through the same disk-cache entries as their own
        # endpoints (PERF-2): the Dashboard's follow-up scans then read what
        # intake measured instead of measuring it again.
        ("quick_stats", lambda: _acache.persistent_call(
            file_path, "quick_stats", None, analyze_quick_stats, file_path, actx)),
        ("signal_integrity", lambda: _acache.persistent_call(
            file_path, "signal_integrity", None, analyze_signal_integrity, file_path, actx)),
        # The intake source is the session's mix/master: mic-capture checks
        # and re-record advice do not apply to it (AID-16).
        ("source_triage", lambda: _acache.persistent_call(
            file_path, "source_triage", triage_cache_params("mix"), triage_source, file_path, actx, "mix")),
        ("pan_balance", lambda: _acache.persistent_call(
            file_path, "pan_balance", {"window_sec": 1.0}, analyze_pan_balance, file_path, 1.0, actx)),
        ("artifacts", lambda: analyze_artifacts(file_path, analysis_ctx=actx)),
        ("musical_context", _musical_context),
        ("stem_context", _stem_context),
    ]

    # Dependency DAG (pipeline enhancement #2): the first five stages are
    # mutually independent (they only read the file / shared context), so they
    # run concurrently. The tail is a dependency chain that must stay serial:
    # musical_context needs source_triage + signal_integrity; stem_context
    # needs musical_context's selected genre.
    independent = stages[:5]
    tail = stages[5:]

    # Warm the shared decodes single-threaded so the concurrent stages read the
    # AnalysisContext cache instead of racing on the first decode (best-effort:
    # if the file can't decode, each stage still records its own failure below).
    from khensu_services.common.perf_trace import span as _span
    try:
        with _span("intake.decode", file=os.path.basename(file_path)):
            actx.load(sr=None, mono=False)
            actx.load(sr=None, mono=True)
    except Exception:
        pass

    results: dict[str, Any] = {}
    status: dict[str, dict[str, Any]] = {}

    def _execute(name: str, fn: Callable[[], Any]) -> tuple[str, bool, Any, str | None]:
        """Run one stage with progress + failure isolation. Returns
        (name, ok, value, error); only _IntakeCancelled propagates. Touches no
        shared dict, so it is safe to run on a worker thread."""
        if progress:
            progress(name, "running")
        try:
            with _span(f"intake.{name}"):    # opt-in perf trace (CE-ENG-4)
                val = fn()
            if progress:
                progress(name, "completed")
            return (name, True, val, None)
        except _IntakeCancelled:
            raise
        except Exception as exc:
            # One broken stage must not abort the bundle (spec §1: failure of
            # one stage never blocks siblings).
            _log.exception("Intake stage %s failed", name)
            if progress:
                progress(name, "failed")
            return (name, False, None, str(exc))

    def _record(name: str, ok: bool, val: Any, err: str | None) -> None:
        if ok:
            results[name] = val
            status[name] = {"ok": True}
        else:
            status[name] = {"ok": False, "error": err}

    # Cancellation checkpoint before the concurrent batch (mirrors the original
    # per-stage "queued" probe).
    for name, _fn in independent:
        if progress and progress(name, "queued") is False:
            raise _IntakeCancelled()

    import concurrent.futures as _cf
    pool = _intake_stage_pool()
    futures = [pool.submit(_execute, name, fn) for name, fn in independent]
    cancelled: _IntakeCancelled | None = None
    for fut in _cf.as_completed(futures):
        try:
            _record(*fut.result())
        except _IntakeCancelled as exc:      # drain the rest before unwinding
            cancelled = exc
    if cancelled is not None:
        raise cancelled

    # Dependency tail runs serially; stem_context also has its own finer-grained
    # cancellation checks internally.
    for name, fn in tail:
        if progress and progress(name, "queued") is False:
            raise _IntakeCancelled()
        _record(*_execute(name, fn))

    # Assemble status/results in declared stage order (concurrency completes out
    # of order) so the bundle is deterministic.
    ordered_status = {name: status[name] for name, _fn in stages if name in status}
    ordered_results = {name: results[name] for name, _fn in stages if name in results}

    ok = sum(1 for s in ordered_status.values() if s["ok"])
    return {
        "file_path": file_path,
        "genre_id": stated_genre_id,
        "selection_mode": selection_mode,
        "stages": [s[0] for s in stages],
        "stage_status": ordered_status,
        "stages_ok": ok,
        "stages_total": len(stages),
        "results": ordered_results,
    }


class IntakeStageOutcome(ResponseModel):
    ok: bool
    error: str | None = None


class IntakeStemContext(ResponseModel):
    """The ``stem_context`` stage: deferred until four separated stems exist,
    else the four stem analyzers' payloads (each its own endpoint's shape)."""
    deferred: bool | None = None
    reason: str | None = None
    arrangement: ArrangementResponse | None = None
    depth: DepthResponse | None = None
    stem_taxonomy: StemTaxonomyResponse | None = None
    masking: MaskingResponse | None = None


class FullIntakeResults(ResponseModel):
    """Per-stage analyzer payloads, keyed by stage name. A stage that failed
    is absent here (its ``stage_status`` entry carries the error); each
    present stage has its own endpoint's shape."""
    quick_stats: QuickStatsResponse | None = None
    signal_integrity: SignalIntegrityResponse | None = None
    source_triage: SourceTriageResponse | None = None
    pan_balance: PanBalanceResponse | None = None
    artifacts: ArtifactScanResponse | None = None
    musical_context: GenreContextResponse | None = None
    stem_context: IntakeStemContext | None = None


class FullIntakeBundle(ResponseModel):
    file_path: str
    genre_id: str | None = None
    selection_mode: str
    stages: list[str]
    stage_status: dict[str, IntakeStageOutcome]
    stages_ok: int
    stages_total: int
    results: FullIntakeResults
    # Present on the synchronous endpoint (cache hit or not), absent inside a
    # job's ``result``.
    cached: bool | None = None


class FullIntakeJobResponse(ResponseModel):
    job_id: str
    state: JobStatus
    stage: str | None = None
    stage_state: JobStatus | None = None
    cancelled: bool
    result: FullIntakeBundle | None = None
    error: str | None = None


class FullIntakeRetryResponse(ResponseModel):
    stage: str
    stage_status: IntakeStageOutcome | None = None
    result: Any = None
    bundle: FullIntakeBundle


@router.post("/analyze/full_intake", response_model=FullIntakeBundle,
             response_model_exclude_unset=True)
async def full_intake_endpoint(req: FullIntakeRequest) -> dict[str, Any]:
    require_file(req.file_path)
    await _require_audio_decode_budget(*_intake_audio_paths(req))

    key = _cache_key(req.file_path, req.stated_genre_id, req.reference_path,
                     req.use_genre_targets, req.confirm_detected_genre,
                     req.selection_mode, req.user_metadata, req.stems)
    cached = _intake_cache.get(key)
    if cached is not None:
        return {**cached, "cached": True}

    try:
        data = await run_blocking(_run_intake, req.file_path, req.stated_genre_id,
                                  req.reference_path, req.use_genre_targets,
                                  req.confirm_detected_genre, req.selection_mode,
                                  req.user_metadata, req.stems)
    except Exception as exc:
        _log.exception("Full intake failed")
        internal_error(cause=exc)

    _intake_cache[key] = data
    return {**data, "cached": False}


# Futures of submitted intake workers (start_job/cancel_job): a cancel before
# the worker starts cancels the future outright.
_intake_futures: dict[str, Future] = {}


def _intake_key(req: FullIntakeRequest) -> tuple:
    return _cache_key(req.file_path, req.stated_genre_id, req.reference_path,
                      req.use_genre_targets, req.confirm_detected_genre,
                      req.selection_mode, req.user_metadata, req.stems)


def _intake_job_worker(job_id: str, req: FullIntakeRequest, cache_key: tuple | None = None) -> None:
    """Background intake job, run on the bounded long-job pool (SVC-04) via
    start_job. SVC-13: the lifecycle goes through common/jobs.py - every write
    under the store lock, a pending "cancelling" is never overwritten by
    "running" (job_begin acknowledges it instead), the terminal write is
    job_settle (which never relabels a shutdown "interrupted"), and a
    completed bundle is cached so the synchronous endpoint reuses it."""
    try:
        if not job_begin(_intake_jobs, job_id):
            return

        def progress(stage: str, state: str) -> bool:
            if job_cancel_requested(_intake_jobs, job_id):
                return False
            job_update(_intake_jobs, job_id, stage=stage, stage_state=state)
            return True

        data = _run_intake(req.file_path, req.stated_genre_id,
                           req.reference_path, req.use_genre_targets,
                           req.confirm_detected_genre, req.selection_mode,
                           req.user_metadata, req.stems, progress)
        if cache_key is not None:
            _intake_cache[cache_key] = data
        job_settle(_intake_jobs, job_id, state=JobStatus.COMPLETED.value, result=data)
    except _IntakeCancelled:
        _mark_cancelled(job_id)
    except Exception:
        _log.exception("Intake job %s failed", job_id)
        job_settle(_intake_jobs, job_id, state=JobStatus.FAILED.value,
                   error="Intake failed - see the service log for details.")


async def _run_intake_job(job_id: str, req: FullIntakeRequest, cache_key: tuple | None = None) -> None:
    """Await one intake job inline on the long-job pool (tests / callers that
    want to wait for it); the start endpoint submits the worker instead."""
    await run_long_blocking(_intake_job_worker, job_id, req, cache_key)


def _mark_cancelled(job_id: str) -> None:
    """Terminal "cancelled" unless shutdown already marked it interrupted."""
    with _intake_jobs._lock:
        job = _intake_jobs.get(job_id)
        if job is not None and job.get("state") != JobStatus.INTERRUPTED.value:
            job["state"] = JobStatus.CANCELLED.value
            job["cancelled"] = True


@router.post("/analyze/full_intake/start", response_model=FullIntakeJobResponse,
             response_model_exclude_unset=True)
async def start_full_intake(req: FullIntakeRequest) -> dict[str, Any]:
    require_file(req.file_path)
    await _require_audio_decode_budget(*_intake_audio_paths(req))
    key = _intake_key(req)
    # Dedup (audit ARCH-01): a double-click (or the desktop re-triggering
    # intake on the same unchanged inputs) joins the already-running pass
    # instead of starting a redundant second one — same identity key already
    # used for the result cache.
    job_id, _created = start_job(
        _intake_jobs, long_job_executor, _intake_job_worker, req, key,
        payload={"state": JobStatus.QUEUED.value, "stage": None,
                 "stage_state": JobStatus.QUEUED.value, "cancelled": False},
        dedup_key=str(key), futures=_intake_futures)
    job = get_job_or_404(_intake_jobs, job_id, detail="Unknown intake job")
    with _intake_jobs._lock:
        return {"job_id": job_id, **job}


@router.get("/analyze/full_intake/status/{job_id}", response_model=FullIntakeJobResponse,
            response_model_exclude_unset=True)
async def full_intake_status(job_id: str) -> dict[str, Any]:
    job = get_job_or_404(_intake_jobs, job_id, detail="Unknown intake job")
    with _intake_jobs._lock:
        return {"job_id": job_id, **job}


@router.post("/analyze/full_intake/cancel/{job_id}", response_model=FullIntakeJobResponse,
             response_model_exclude_unset=True)
async def cancel_full_intake(job_id: str) -> dict[str, Any]:
    job = get_job_or_404(_intake_jobs, job_id, detail="Unknown intake job")
    # cancel_job marks a queued/running job "cancelling" under the store lock;
    # the worker acknowledges it ("cancelled") only after it unwinds, so a
    # terminal cancellation is never reported while _run_intake still runs.
    cancel_job(_intake_jobs, job_id, _intake_futures)
    with _intake_jobs._lock:
        job["cancelled"] = True
        return {"job_id": job_id, **job}


@router.post("/analyze/full_intake/retry/{stage}", response_model=FullIntakeRetryResponse,
             response_model_exclude_unset=True)
async def retry_full_intake_stage(stage: str, req: FullIntakeRequest) -> dict[str, Any]:
    """Invalidate the bundle and rerun it, returning the requested stage.

    Stages share prerequisites (musical context needs preflight; stem context
    needs musical context), so retrying safely rebuilds the dependency DAG
    rather than serving a stale mixed-generation result.
    """
    known = {"quick_stats", "signal_integrity", "source_triage", "pan_balance",
             "artifacts", "musical_context", "stem_context"}
    if stage not in known:
        not_found(f"Unknown intake stage: {stage}")
    # 404 before _cache_key's os.stat (which would otherwise surface as a 500).
    if not os.path.isfile(req.file_path):
        not_found(f"File not found: {req.file_path}")
    key = _cache_key(req.file_path, req.stated_genre_id, req.reference_path,
                     req.use_genre_targets, req.confirm_detected_genre,
                     req.selection_mode, req.user_metadata, req.stems)
    _intake_cache.pop(key, None)
    bundle = await full_intake_endpoint(req)
    return {"stage": stage, "stage_status": bundle["stage_status"].get(stage),
            "result": bundle["results"].get(stage), "bundle": bundle}


