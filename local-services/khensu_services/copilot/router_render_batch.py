"""Batch chain renders over a folder: /apply_chain/batch/* (ENG-6, from router_render.py)."""
from __future__ import annotations

import logging
import os
from fastapi import APIRouter
from pydantic import Field, field_validator
from khensu_services.common.api_models import RequestModel
from khensu_services.common.job_status import JobStatus
from khensu_services.common.errors import conflict, invalid_input, not_found
from khensu_services.common.jobs import JobStore, get_job_or_404, job_begin, job_cancel_requested, job_settle, request_fingerprint, start_job
from khensu_services.common.executors import long_job_executor as _long_executor
from khensu_services.copilot.router_plugin_matching import EffectSpec
from khensu_services.copilot.response_models import BatchJobStartResponse, BatchJobStatusResponse, CopilotJobCancelResponse
from khensu_services.copilot.router_render import PostRenderGuardrailError, RENDER_STATE_BLOCKED, RENDER_STATE_VALIDATED, RenderBlocked, RenderCancelled, RenderGraph, _cancel_reply, _chain_render_fn, _decision_hash_field, _effect_dicts, _file_identity, _genre_action_block, _graph_dict, _log_applied_event, _require_audio_decode_budget, _require_valid_parameters, render_through_pipeline, resolve_key_stem, warnings_kw

_log = logging.getLogger(__name__)
router = APIRouter()


# ---------------------------------------------------------------------------
# Batch apply_chain — the same render applied to every audio file in a folder
# (or an explicit file list). Generalizes the album pipeline's many-files idea
# from analysis to processing: one chain, N files, per-file success/failure so
# a single corrupt take doesn't sink the batch. Same start/status/cancel shape
# as the other job queues; cancellation is checked between files, so an
# in-flight single-file render finishes but no further files start.
# ---------------------------------------------------------------------------
_AUDIO_BATCH_EXTS = {".wav", ".flac", ".aif", ".aiff", ".ogg", ".mp3"}


_BATCH_MAX_FILES = 200


_batch_jobs = JobStore(name="apply_chain_batch", max_active=3)  # bounded (F2) + admission cap (ARCH-01)


_batch_futures: dict = {}


class ApplyChainBatchRequest(RequestModel):
    input_dir: str = ""                              # folder to scan (non-recursive)
    file_paths: list[str] = Field(default_factory=list, max_length=500)  # and/or explicit files
    output_dir: str = ""    # default: <input_dir or first file's dir>/khensu_batch
    suffix: str = "_processed"                       # appended before the .wav extension
    effects: list[EffectSpec] = Field(default_factory=list, max_length=64)
    graph: RenderGraph | None = None
    normalize: bool = False
    session_id: str = ""
    decision_hash: str = _decision_hash_field()
    genre_relative: bool = False
    genre_action_allowed: bool = False
    abstained_reason: str = ""
    # Optional stem-targeted batch metadata. A batch may not claim stem
    # processing without an explicit source manifest for every declared stem.
    stem_paths: dict[str, str] = Field(default_factory=dict)
    stem_provenance: dict[str, dict[str, str]] = Field(default_factory=dict)
    file_stem_paths: dict[str, dict[str, str]] = Field(default_factory=dict)
    # Which stem the chain targets; every file is attribution-checked against
    # its own stem mapping exactly like the single-file routes (CPL-01).
    target_stem: str = "mix"
    # ENH-6: as ApplyChainRequest.key_stem, resolved per file through that
    # file's stem mapping (file_stem_paths, else stem_paths).
    key_stem: str = Field(default="", max_length=64)

    @field_validator("suffix")
    @classmethod
    def _require_plain_suffix(cls, value: str) -> str:
        """The suffix is spliced into a filename: it must not be able to steer
        the output out of output_dir (separators, '..', drive colons) or carry
        control characters."""
        if len(value) > 64:
            raise ValueError("suffix too long (max 64 characters)")
        if any(ch in value for ch in ("/", "\\", ":")) or ".." in value:
            raise ValueError("suffix must not contain path separators, '..' or ':'")
        if any(ord(ch) < 32 or ord(ch) == 127 for ch in value):
            raise ValueError("suffix must not contain control characters")
        return value


def _run_batch_job(job_id: str, req: ApplyChainBatchRequest,
                   files: list[str], output_dir: str) -> None:
    job = _batch_jobs.get(job_id)
    if job is None or not job_begin(_batch_jobs, job_id):
        return
    try:
        _run_batch_files(job_id, job, req, files, output_dir)
    except Exception as exc:
        # Anything outside the per-file try (makedirs, bookkeeping) must not
        # leave the job stuck "running" forever.
        _log.exception("Batch apply_chain job %s failed", job_id)
        job_settle(_batch_jobs, job_id, status=JobStatus.FAILED.value, error=f"Batch failed: {exc}")
        return
    # Never overwrite a cancellation or a shutdown interruption with success
    # (job_settle acknowledges a pending "cancelling" as "cancelled").
    job_settle(_batch_jobs, job_id, status=JobStatus.COMPLETED.value)


def _run_batch_files(job_id: str, job: dict, req: ApplyChainBatchRequest,
                     files: list[str], output_dir: str) -> None:
    """Every file goes through the same pipeline as a single-file render
    (audit CPL-01 / SVC-01): stem attribution, the pre-render validator,
    a quarantined render, the post-render guardrail, then promotion. Each
    entry records its own ``render_state``; a blocked or rejected file never
    reaches ``output_dir`` under its final name and is never logged as
    applied."""
    from khensu_services.common.output_paths import reject_output_alias, validate_output_path
    from khensu_services.copilot.render_validator import validate_stem_attribution
    os.makedirs(output_dir, exist_ok=True)

    graph = _graph_dict(req.graph)
    effect_list = _effect_dicts(req.effects)
    used_names: set[str] = set()

    def _cancelled() -> bool:
        return job_cancel_requested(_batch_jobs, job_id)

    for src in files:
        if _cancelled():
            return
        stem = os.path.splitext(os.path.basename(src))[0]
        out_name = f"{stem}{req.suffix}.wav"
        # Same stem under different extensions must not overwrite each other.
        n = 2
        while out_name in used_names:
            out_name = f"{stem}{req.suffix}_{n}.wav"
            n += 1
        used_names.add(out_name)
        out_path = os.path.join(output_dir, out_name)

        entry: dict = {"file_path": src, "output_path": out_path}
        file_stems = req.file_stem_paths.get(src, req.stem_paths)
        if req.stem_paths:
            entry["stem_provenance"] = req.stem_provenance
            entry["stem_paths"] = file_stems
        try:
            # Per-file containment (not just the directory probe at start):
            # the resolved path must stay within the allowed roots and must
            # never alias the source it was rendered from.
            validate_output_path(out_path, src)
            reject_output_alias(out_path, src, *file_stems.values())
            # An unreadable/corrupt entry keeps its documented per-file
            # "failed" semantics (not a validator "blocked").
            import soundfile as _sf
            _sf.info(src)
            attribution = validate_stem_attribution(src, req.target_stem, file_stems)
            if not attribution["ok"]:
                raise RenderBlocked(attribution)
            key_path, key_warnings = resolve_key_stem(req.key_stem, file_stems)
            result, guardrail, _validation = render_through_pipeline(
                src, out_path, _chain_render_fn(src, effect_list, graph, req.normalize, key_path),
                effects=None if graph else effect_list, graph=graph, should_cancel=_cancelled)
            _log_applied_event("chain_applied", req.session_id, "",
                               out_path, result.get("applied", []),
                               graph if graph else effect_list, req.decision_hash)
            entry.update(status="success", render_state=RENDER_STATE_VALIDATED,
                         applied_effects=result.get("applied", []),
                         skipped_effects=result.get("skipped", []),
                         guardrail=guardrail,
                         **warnings_kw(key_warnings + list(result.get("warnings") or [])))
        except RenderCancelled:
            return
        except RenderBlocked as blocked:
            entry.update(status="blocked", render_state=RENDER_STATE_BLOCKED, error=str(blocked),
                         guardrail={"pre_render": blocked.validation})
            job["progress"]["failed"] += 1
        except PostRenderGuardrailError as exc:
            _log.warning("Batch render rejected by post-render guardrail for %s (job %s): %s",
                         src, job_id, exc)
            entry.update(status="rejected", render_state=exc.render_state, error=str(exc),
                         quarantine_path=exc.quarantine_path)
            if isinstance(exc.measurements, dict):
                entry["guardrail"] = exc.measurements
            job["progress"]["failed"] += 1
        except Exception as exc:
            _log.exception("Batch apply_chain failed for %s (job %s)", src, job_id)
            detail = getattr(exc, "detail", None)
            entry.update(status=JobStatus.FAILED.value, error=str(detail) if detail else str(exc))
            job["progress"]["failed"] += 1
        # done counts files processed (success or failure) — it's the progress
        # bar numerator; failed is the subset that did not produce a render.
        job["progress"]["done"] += 1
        job["results"].append(entry)


@router.post("/apply_chain/batch/start", response_model=BatchJobStartResponse, response_model_exclude_unset=True)
async def apply_chain_batch_start(req: ApplyChainBatchRequest) -> dict:
    blocked_reason = _genre_action_block(req)
    if blocked_reason:
        conflict(blocked_reason)
    if req.stem_paths:
        missing = sorted(set(req.stem_paths) - set(req.stem_provenance))
        if missing:
            invalid_input("Stem provenance required for: " + ", ".join(missing))
        invalid = [name for name, path in req.stem_paths.items()
                   if not os.path.isfile(path) or not req.stem_provenance[name].get("source_id")
                   or not req.stem_provenance[name].get("method")]
        if invalid:
            invalid_input("Stem paths and provenance are invalid: " + ", ".join(sorted(invalid)))
    # The chain itself is checked before admission (CPL-01): a malformed
    # parameter is a 422 now, not N failed files later. Layout-dependent
    # checks run per file inside the job.
    _require_valid_parameters(None if req.graph else _effect_dicts(req.effects), _graph_dict(req.graph))
    files: list[str] = []
    if req.input_dir:
        if not os.path.isdir(req.input_dir):
            not_found(f"Folder not found: {req.input_dir}")
        for name in sorted(os.listdir(req.input_dir)):
            p = os.path.join(req.input_dir, name)
            if os.path.isfile(p) and os.path.splitext(name)[1].lower() in _AUDIO_BATCH_EXTS:
                files.append(p)
    for p in req.file_paths:
        if not os.path.isfile(p):
            not_found(f"File not found: {p}")
        if p not in files:
            files.append(p)

    if not files:
        invalid_input("No audio files to process (empty folder / file list)")
    if len(files) > _BATCH_MAX_FILES:
        invalid_input(f"Batch too large: {len(files)} files (max {_BATCH_MAX_FILES})")
    await _require_audio_decode_budget(*files, allow_probe_errors=True)
    if req.key_stem:
        # Every key a file would read is a render input too (ENH-6); an
        # unknown or missing one falls back per file with a warning.
        key_paths = {resolve_key_stem(req.key_stem, req.file_stem_paths.get(f, req.stem_paths))[0]
                     for f in files}
        await _require_audio_decode_budget(*sorted(k for k in key_paths if k), allow_probe_errors=True)
    if req.stem_paths and req.file_stem_paths:
        unknown_files = sorted(set(req.file_stem_paths) - set(files))
        if unknown_files:
            invalid_input("Stem mapping references unknown files: " + ", ".join(unknown_files))
        for file_path, mapping in req.file_stem_paths.items():
            if set(mapping) != set(req.stem_paths):
                invalid_input(f"Stem mapping incomplete for {file_path}")

    base_dir = req.input_dir or os.path.dirname(files[0])
    output_dir = req.output_dir or os.path.join(base_dir, "khensu_batch")
    # Containment check once for the directory; every per-file output stays
    # inside it (parity with the single-file endpoint's validate_output_path).
    from khensu_services.common.output_paths import validate_output_path
    validate_output_path(os.path.join(output_dir, "probe.wav"), files[0])

    # Dedup (audit ARCH-01): the same resolved file set + output_dir joins
    # the already-running batch instead of starting a redundant second one -
    # but only while every source is unchanged (mtime+size, SVC-07).
    dedup_key = (output_dir + "|" + ",".join(sorted(files)) + "|"
                 + request_fingerprint(req, _file_identity(*sorted(files))))
    # Long job: its own pool, so a batch never pins the interactive DSP workers.
    job_id, _created = start_job(_batch_jobs, _long_executor, _run_batch_job, req, files, output_dir,
                                 futures=_batch_futures, payload={
        "status": JobStatus.PENDING.value,
        "progress": {"total": len(files), "done": 0, "failed": 0},
        "results": [],
        "error": None,
        "stem_provenance": req.stem_provenance if req.stem_paths else {},
        "file_stem_paths": req.file_stem_paths if req.stem_paths else {},
        # Batch renders often run without a session (so no provenance event):
        # keep the model decision on the job itself.
        "decision_hash": req.decision_hash,
    }, dedup_key=dedup_key)
    return {"job_id": job_id, "total_files": len(files)}


@router.get("/apply_chain/batch/status/{job_id}", response_model=BatchJobStatusResponse, response_model_exclude_unset=True)
async def apply_chain_batch_status(job_id: str) -> dict:
    return get_job_or_404(_batch_jobs, job_id)


@router.post("/apply_chain/batch/{job_id}/cancel", response_model=CopilotJobCancelResponse, response_model_exclude_unset=True)
async def apply_chain_batch_cancel(job_id: str) -> dict:
    return _cancel_reply(_batch_jobs, job_id, _batch_futures)
