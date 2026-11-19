from __future__ import annotations
import logging
import os

from fastapi import APIRouter, HTTPException

from khensu_services.analysis.models import (
    AnalyzeRequest, AnalyzeResponse, ExtendedAnalysisResponse,
    MonoCompatResult, MidSideImbalanceResult, ResonanceHotspot, VocalClarityResult,
    ReferenceAnalysisRequest, ReferenceAnalysisResponse, ReferenceTimeWindow,
    SectionsAnalysisRequest, SectionsAnalysisResponse, SectionAnalysisResult,
    LoudnessArcResult,
    DrumAnalysisRequest, DrumAnalysisResponse, DrumPartResult, DrumEventResult,
    FileIntegrityRequest, FileIntegrityResult,
    ChordsRequest, ChordsResponse, ChordEvent,
    VocalRequest, VocalAnalysisResponse,
    LyricsRequest, LyricAnalysisResponse,
    EmotionalArcData, EmotionalArcPoint, GenreBlendEntry,
    TranslationPredictionData, TranslationProfileScore, TranslationCollision,
    BenchmarkData, StereoFieldTimeline, StereoFieldPoint,
    TrackMood,
)
from khensu_services.analysis.section_analyzer import compute_loudness_arc
from khensu_services.common.job_status import JobStatus
from khensu_services.analysis.extended_analyzer import (
    analyze_harshness, analyze_sibilance, analyze_vocal_clarity,
    analyze_mono_compat, analyze_mid_side_imbalance,
    analyze_translation_risk, analyze_resonances,
    analyze_stereo_field_over_time,
)
from khensu_services.analysis.mood_energy import analyze_emotional_arc
from khensu_services.analysis.genre_detector import genre_blend_for_file
from khensu_services.analysis.translation_analyzer import analyze_translation
from khensu_services.analysis.benchmark import benchmark_against_references
from khensu_services.common.executors import run_blocking, run_io_blocking
from khensu_services.version import SERVICE_VERSION
from khensu_services.analysis import _lean_spectral as _lean

from concurrent.futures import Future
from khensu_services.common.errors import (
    internal_error,
    invalid_input,
    not_found,
    require_decode_budget,
    unavailable,
    undecodable_audio_cause,
)
from khensu_services.common.executors import dsp_executor as _executor
from khensu_services.common.jobs import (
    JobStore, admit_or_join_or_429, cancel_reply, get_job_or_404, job_begin, job_settle,
    job_update, new_job_id, request_fingerprint, track_future,
)
from khensu_services.common.api_models import ResponseModel
from khensu_services.common.job_models import (
    JobCancelResponse, JobIdResponse, StageJobStatusResponse,
)
from khensu_services.common.lru import BoundedCache, file_cache_key
from khensu_services.common.single_flight import SingleFlight
from khensu_services.common import analysis_cache as _acache
from khensu_services.analysis.context import load_audio as _load_audio

# max_active caps only jobs actually submitted to the executor — the
# cache-hit fast path below (an already-"completed" job) never touches it and
# so is admitted directly (audit ARCH-01).
_analyze_jobs = JobStore(name="analyze", max_active=6)


# Cache keyed by (file content SHA-256, mode) — re-analyzing an unchanged file
# (e.g. reopening the same session) skips straight to the cached result
# instead of re-running a 15-60s advanced pass. compute_sha256() only reads
# the file in 64KB chunks (no audio decode), so hashing is cheap relative to
# the analysis it guards. This is the in-memory tier; the persistent tier
# behind it (common/analysis_cache, CE-PLT-2) survives restarts. Bounded so a
# session with many distinct files doesn't retain every full analysis bundle
# for the life of the process.
_analysis_cache: BoundedCache[tuple[str, str], dict] = BoundedCache(max_entries=32)
# Extended results (base + extras, JSON-safe dicts), same key. Smaller: each
# entry is a superset of the base bundle.
_extended_cache: BoundedCache[tuple[str, str], dict] = BoundedCache(max_entries=16)
_analyze_futures: dict[str, Future] = {}
_chords_cache: BoundedCache[tuple, dict] = BoundedCache(max_entries=32)
# Lyric scans stay in memory (FIX-17): the transcript depends on the Whisper
# weights installed, which a content-hash key cannot see, so a stored entry
# could outlive the model that produced it. Keyed on the file stamp and the
# model size; concurrent requests for one key share one transcription.
_lyrics_cache: BoundedCache[tuple, dict] = BoundedCache(max_entries=16)
_lyrics_flights: SingleFlight[dict] = SingleFlight()


def _analysis_cache_key(file_path: str, mode: str) -> tuple[str, str] | None:
    try:
        # The same raw-bytes sha256 the persistent cache keys on, memoised
        # per (path, mtime, size) so a second request doesn't re-read the file.
        return (_acache.content_hash(file_path), mode)
    except Exception:
        return None  # best-effort — falls back to always re-analyzing


# --- CE-PLT-2: persistent (disk) tier behind the in-memory caches ----------
# The in-memory BoundedCaches above die with the process; the content-
# addressed store in common/analysis_cache survives restarts, so reopening a
# session after a relaunch is a file read rather than a 15-60 s pass.
# Each analyzer's version is declared in common/analysis_versions (FIX-15):
# bump "analyze_base" / "analyze_extended" there when analyze_quick /
# analyze_advanced / the extended extras change.


def _disk_key(cache_key: tuple[str, str] | None, analyzer: str) -> str | None:
    if cache_key is None or not _acache.enabled():
        return None
    params: dict = {"mode": cache_key[1]}
    if analyzer in ("analyze_base", "analyze_extended"):
        # The genre fields depend on whether the CLAP genre probe can run.
        from khensu_services.analysis.learned import genre_probe
        params["genre"] = genre_probe.cache_tag()
        # NEW-9: the whole-track mood depends on whether the CLAP mood probe can run.
        from khensu_services.analysis.learned import clap_probes
        params["mood"] = clap_probes.cache_tag(clap_probes.MOOD_FILE)
    return _acache.make_key([cache_key[0]], analyzer, _acache.version_of(analyzer), params)


def _base_get(cache_key: tuple[str, str] | None) -> dict | None:
    """Blocking: the base analysis from memory, else from disk (promoted)."""
    if cache_key is None:
        return None
    hit = _analysis_cache.get(cache_key)
    if hit is None:
        hit = _acache.lookup("analyze_base", _disk_key(cache_key, "analyze_base"))
        if hit is not None:
            _analysis_cache[cache_key] = hit
    return hit


def _base_put(cache_key: tuple[str, str] | None, data: dict) -> None:
    if cache_key is None:
        return
    _analysis_cache[cache_key] = data
    _acache.store("analyze_base", _disk_key(cache_key, "analyze_base"), data)


def _ext_get(cache_key: tuple[str, str] | None) -> dict | None:
    if cache_key is None:
        return None
    hit = _extended_cache.get(cache_key)
    if hit is None:
        hit = _acache.lookup("analyze_extended", _disk_key(cache_key, "analyze_extended"))
        if hit is not None:
            _extended_cache[cache_key] = hit
    return hit


def _ext_put(cache_key: tuple[str, str] | None, data: dict) -> None:
    if cache_key is None:
        return
    _extended_cache[cache_key] = data
    _acache.store("analyze_extended", _disk_key(cache_key, "analyze_extended"), data)


def cached_analyze_quick(file_path: str, ctx=None) -> dict:
    """Blocking: ``analyze_quick`` through both cache tiers - the one pass
    /analyze (quick), /analyze/sections and full intake share (PERF-2: it
    used to run three times per intake)."""
    import khensu_services.analysis.router as main_router
    key = _analysis_cache_key(file_path, "quick")
    hit = _base_get(key)
    if hit is not None:
        return hit
    data = main_router.analyze_quick(file_path, ctx=ctx) if ctx is not None         else main_router.analyze_quick(file_path)
    _base_put(key, data)
    return data


_log = logging.getLogger(__name__)
router = APIRouter()


async def _require_audio_decode_budget(*paths: str) -> None:
    """Reject unsafe decode inputs before they occupy an analyzer worker."""
    await require_decode_budget(*paths)


@router.post("/analyze", response_model=AnalyzeResponse, response_model_exclude_unset=True)
async def analyze(req: AnalyzeRequest) -> AnalyzeResponse:
    if not os.path.isfile(req.file_path):
        not_found(f"File not found: {req.file_path}")
    await _require_audio_decode_budget(req.file_path)

    cache_key = await run_blocking(_analysis_cache_key, req.file_path, req.mode)
    # One lookup, not `in` + index: the entry can be evicted between the two.
    cached = await run_io_blocking(_base_get, cache_key)
    if cached is not None:
        return AnalyzeResponse(**cached)

    import khensu_services.analysis.router as main_router
    try:
        if req.mode == "quick":
            data = await run_blocking(main_router.analyze_quick, req.file_path)
        else:
            data = await run_blocking(main_router.analyze_advanced, req.file_path)
    except Exception as exc:
        _log.exception("Analyzer failed for %s", req.file_path)
        internal_error(cause=exc)

    await run_io_blocking(_base_put, cache_key, data)
    return AnalyzeResponse(**data)


# Advanced /analyze/start jobs compute the EXTENDED result (the base pass
# plus harshness / translation / genre blend / ...), so the desktop no
# longer has to follow the job with a second /analyze/extended that re-ran
# everything. The base pass reports stages 1..9; the extended extras are
# stage 10.
_ADVANCED_JOB_STAGES = 10
_QUICK_JOB_STAGES = 9


@router.post("/analyze/start", response_model=JobIdResponse, response_model_exclude_unset=True)
async def analyze_start(req: AnalyzeRequest) -> dict:
    """Start an analysis job. ``result`` of a completed job is the base
    AnalyzeResponse for ``quick`` mode and the full ExtendedAnalysisResponse
    (``model_dump(mode="json")`` shape) for ``advanced`` mode."""
    if not os.path.isfile(req.file_path):
        not_found(f"File not found: {req.file_path}")
    await _require_audio_decode_budget(req.file_path)

    import khensu_services.analysis.router as main_router

    extended = req.mode == "advanced"
    # A fix check (FEAT-1) skips the extras no finding reads; a full result
    # already cached for the file serves it too.
    fix_check = extended and req.purpose == "fix_check"
    total = _ADVANCED_JOB_STAGES if extended else _QUICK_JOB_STAGES
    job_id = new_job_id()
    cache_key = await run_blocking(_analysis_cache_key, req.file_path, req.mode)

    cached_result = None
    cached_base = None
    if cache_key is not None:
        cached_base = await run_io_blocking(_base_get, cache_key)
        cached_result = (await run_io_blocking(_ext_get, cache_key)) if extended else cached_base
        if cached_result is None and fix_check:
            cached_result = await run_io_blocking(_ext_get, _fix_check_key(cache_key))
    if cached_result is not None:
        # Unchanged file — skip straight to "completed" instead of re-running
        # a 15-60s advanced pass; the client's usual poll loop just sees an
        # already-finished job on its first check. This never touches the
        # executor, so it's admitted directly rather than through the cap.
        _analyze_jobs[job_id] = {
            "status": JobStatus.COMPLETED.value, "stage": "cached", "stage_index": total,
            "total_stages": total, "result": cached_result, "error": None,
        }
        return {"job_id": job_id}

    # Dedup (audit ARCH-01): a double-click on the same (file, mode) joins
    # the already-running analysis instead of starting a redundant second
    # pass. The file's mtime+size are part of the identity (AUDIT-072): a
    # file re-rendered in place mid-analysis must not join the stale run.
    st = os.stat(req.file_path)
    dedup_key = request_fingerprint(req.file_path, req.mode, req.purpose if extended else "full",
                                    st.st_mtime_ns, st.st_size)
    job_id, created = admit_or_join_or_429(_analyze_jobs, job_id, {
        "status": JobStatus.PENDING.value, "stage": "queued", "stage_index": 0,
        "total_stages": total, "result": None, "error": None,
    }, dedup_key)
    if not created:
        return {"job_id": job_id}

    def _run():
        if not job_begin(_analyze_jobs, job_id):
            return

        def cb(stage: str, idx: int, _total: int) -> None:
            job_update(_analyze_jobs, job_id, stage=stage, stage_index=idx, total_stages=total)

        from khensu_services.common.perf_trace import span as _span
        try:
            with _span("analyze.job", mode=req.mode):    # opt-in perf trace (CE-ENG-4)
                if extended:
                    data = compute_extended_result(req.file_path, req.mode, base_result=cached_base,
                                                   progress_cb=cb, cache_key=cache_key,
                                                   extras=not fix_check)
                else:
                    data = main_router.analyze_quick(req.file_path)
                    _base_put(cache_key, data)
            job_settle(_analyze_jobs, job_id, status=JobStatus.COMPLETED.value, result=data,
                       stage="done", stage_index=total)
        except Exception as exc:
            _log.exception("Background analysis failed for job %s", job_id)
            job_settle(_analyze_jobs, job_id, status=JobStatus.FAILED.value, error=str(exc))
    # Cleanup via done-callback (fires immediately if already done): covers a
    # future cancelled before it ran, and a worker finishing before this
    # assignment (the old pop-in-finally could run first, leaving a stale entry).
    track_future(_analyze_futures, job_id, _executor.submit(_run))
    return {"job_id": job_id}


@router.get("/analyze/status/{job_id}", response_model=StageJobStatusResponse,
            response_model_exclude_unset=True)
async def analyze_status(job_id: str) -> dict:
    return get_job_or_404(_analyze_jobs, job_id)


@router.post("/analyze/{job_id}/cancel", response_model=JobCancelResponse,
             response_model_exclude_unset=True)
async def analyze_cancel(job_id: str) -> dict:
    # A queued future can be cancelled before it consumes a worker. A running
    # third-party library call cannot safely be interrupted from another
    # thread, so it remains visibly "cancelling" until the worker returns;
    # only then does the job become terminally cancelled.
    return cancel_reply(_analyze_jobs, job_id, futures=_analyze_futures)


def _fix_check_key(cache_key: tuple[str, str] | None) -> tuple[str, str] | None:
    """Where a fix-check extended result is cached: apart from the full
    result, which it lacks the extras of."""
    return None if cache_key is None else (cache_key[0], cache_key[1] + "+fix_check")


def compute_extended_result(file_path: str, mode: str = "advanced",
                            base_result: dict | None = None,
                            progress_cb=None,
                            cache_key: tuple[str, str] | None = None,
                            extras: bool = True) -> dict:
    """Blocking: the full extended analysis as a JSON-safe dict (the shape of
    ``ExtendedAnalysisResponse.model_dump(mode="json")``).

    ``base_result`` (a cached quick/advanced analysis of the same file)
    skips the base pass; otherwise it is computed (advanced reports
    ``progress_cb`` stages 1..9) and stored in ``_analysis_cache``. The
    extras are stage 10. With ``cache_key`` the result is also stored in
    ``_extended_cache`` so /analyze/extended and later jobs reuse it.

    ``extras=False`` (a fix check, FEAT-1) leaves out what no finding is
    built from - the emotional arc, translation prediction, stereo-field
    timeline and benchmark - and caches the result under
    ``_fix_check_key(cache_key)``, never as the full result.
    """
    import numpy as np
    import khensu_services.analysis.router as main_router

    from khensu_services.common.perf_trace import span as _span
    if base_result is None:
        with _span("analyze.base", mode=mode):
            if mode == "quick":
                base_result = main_router.analyze_quick(file_path)
            else:
                base_result = main_router.analyze_advanced(file_path, progress_cb=progress_cb)
        _base_put(cache_key, base_result)

    with _span("analyze.extended_decode"):
        y_stereo, sr = _load_audio(file_path, sr=None, mono=False)
    # Crest per channel on the real layout, before mono is duplicated.
    from khensu_services.analysis.loudness import crest_factor_db
    crest = crest_factor_db(y_stereo)
    if y_stereo.ndim == 1:
        y_stereo = np.stack([y_stereo, y_stereo])

    sr_int       = int(sr)
    harsh_result = analyze_harshness(y_stereo, sr_int)
    sib_result   = analyze_sibilance(y_stereo, sr_int)
    vocal_result = analyze_vocal_clarity(y_stereo, sr_int)
    mono_result  = analyze_mono_compat(y_stereo, sr_int)
    ms_result    = analyze_mid_side_imbalance(y_stereo, sr_int)
    res_hotspots = analyze_resonances(y_stereo, sr_int)

    y_mono_a = y_stereo.mean(axis=0)
    centroid_arr = _lean.spectral_centroid(y_mono_a, sr_int)
    centroid_hz  = float(np.mean(centroid_arr))
    from khensu_services.analysis.dsp_metrics import low_band_energy_ratio
    spectral_data  = {"centroid_hz": centroid_hz,
                      # Welch PSD, not a full-length rfft of the whole track.
                      "balance_low": low_band_energy_ratio(y_mono_a, sr_int, 250.0)}
    # The base result carries no crest factor, so the old
    # base_result.get("crest_factor_db", 10.0) always read 10 dB and the
    # translation over-compression term could never fire (audit M3).
    dynamics_data  = {"crest_factor_db": float(crest) if crest is not None else 10.0}
    trans_risk     = analyze_translation_risk(mono_result, spectral_data, dynamics_data)

    # --- 2026-07-09 AI-analysis-deepening additions ---
    bpm = float(base_result.get("bpm", 120.0))
    key = str(base_result.get("key", "C major"))

    emotional_arc = None
    if extras:
        arc_result = analyze_emotional_arc(y_mono_a, sr_int, bpm, key)
        emotional_arc = EmotionalArcData(
            points=[EmotionalArcPoint(**p) for p in arc_result["points"]],
            flat_sections=arc_result["flat_sections"],
            max_energy_delta=arc_result["max_energy_delta"],
        )
        # NEW-9: the CLAP mood probe's whole-track tags, when it can run.
        from khensu_services.analysis.learned import clap_probes
        try:
            mood = clap_probes.predict_mood(file_path)
        except Exception:  # noqa: BLE001 - an optional model never fails the analysis
            _log.debug("CLAP mood probe failed", exc_info=True)
            mood = None
        if mood is not None:
            emotional_arc.track_mood = TrackMood(**mood)

    # NEW-9: the CLAP probe when it can run, else the feature-distance ranker
    # (which named the wrong family more often than chance on the corpus).
    blend_result, _genre_pred = genre_blend_for_file(file_path, y_mono_a, sr_int, bpm)
    genre_blend = [GenreBlendEntry(**e) for e in blend_result]

    # Genre provenance + confirmation (safety audit P1): the detected
    # genre is a candidate, the target catalog is hand-tuned. Surface
    # this so consumers don't auto-apply genre-relative settings.
    from khensu_services.analysis.genre_provenance import (
        GENRE_TARGET_PROVENANCE, genre_confirmation as _genre_confirmation,
        genre_relative_abstention_reason,
    )
    genre_provenance = dict(GENRE_TARGET_PROVENANCE)
    genre_conf = _genre_confirmation(blend_result, user_confirmed=False)

    translation_prediction = None
    stereo_field_timeline = None
    if extras:
        trans_pred_result = analyze_translation(y_mono_a, sr_int)
        translation_prediction = TranslationPredictionData(
            per_profile={
                device: TranslationProfileScore(
                    degradation_score=p["degradation_score"],
                    new_masking_collisions=[
                        TranslationCollision(**c) for c in p["new_masking_collisions"]
                    ],
                )
                for device, p in trans_pred_result["per_profile"].items()
            },
        )

        stereo_result = analyze_stereo_field_over_time(y_stereo, sr_int)
        stereo_field_timeline = StereoFieldTimeline(
            points=[StereoFieldPoint(**p) for p in stereo_result["points"]],
        )

    # Benchmark against the reference library using the primary
    # highest-ranked detected genre — the library only has
    # entries for 5 hand-tuned genres (shoegaze/ambient/hyperpop/
    # dream-pop/lo-fi-hip-hop), so this is None for the rest. Note
    # estimate_genre_blend's vocabulary (pop/rock/hiphop/electronic/
    # rnb) is coarser than those niche ids today, so in practice this
    # rarely fires yet — expanding genre detection to the same
    # granularity as the reference library is a follow-up, not
    # addressed here.
    benchmark = None
    if genre_blend and extras:
        primary_genre_id = genre_blend[0].genre
        metrics = _extract_mix_metrics(file_path)
        tilt = metrics["tilt_db_per_octave"]   # shared dsp_metrics tilt
        bench_result = benchmark_against_references(
            primary_genre_id, metrics["lufs"], metrics["lra"], tilt, metrics["crest_db"],
        )
        if bench_result is not None:
            benchmark = BenchmarkData(**bench_result)

    response = ExtendedAnalysisResponse(
        **base_result,
        harshness_score=harsh_result["score"],
        harshness_peak_hz=harsh_result.get("peak_hz"),
        harshness_peak_db=harsh_result.get("peak_db"),
        sibilance_score=sib_result["score"],
        sibilance_peak_hz=sib_result.get("peak_hz"),
        vocal_clarity=VocalClarityResult(**vocal_result),
        mono_compat=MonoCompatResult(
            per_band_correlation=mono_result["per_band_correlation"],
            low_end_stereo_pct=mono_result["low_end_stereo_pct"],
        ),
        mid_side_imbalance=MidSideImbalanceResult(**ms_result),
        translation_risk=trans_risk["per_environment"],
        resonance_hotspots=[
            ResonanceHotspot(hz=h["hz"], db=h["db"], prominence_db=h["prominence"])
            for h in res_hotspots["hotspots"]
        ],
        emotional_arc=emotional_arc,
        genre_blend=genre_blend,
        genre_provenance=genre_provenance,
        genre_confirmation=genre_conf,
        genre_abstained_reason=genre_relative_abstention_reason(
            blend_result, user_confirmed=False),
        genre_relative_action_allowed=False,
        genre_is_hybrid=False,
        translation_prediction=translation_prediction,
        benchmark=benchmark,
        stereo_field_timeline=stereo_field_timeline,
    )
    data = response.model_dump(mode="json")
    if progress_cb is not None:
        progress_cb("extended_metrics", _ADVANCED_JOB_STAGES, _ADVANCED_JOB_STAGES)
    _ext_put(cache_key if extras else _fix_check_key(cache_key), data)
    return data


@router.post("/analyze/extended", response_model=ExtendedAnalysisResponse, response_model_exclude_unset=True)
async def analyze_extended(req: AnalyzeRequest) -> ExtendedAnalysisResponse:
    """Run standard analysis + harshness + mono compat + translation risk + resonance.

    Kept for older clients: newer ones read the same result from a completed
    advanced /analyze/start job. Reuses a cached extended result, or a cached
    base analysis of the unchanged file, instead of recomputing."""
    import pathlib

    file_path = pathlib.Path(req.file_path)
    if not file_path.exists():
        not_found(f"File not found: {req.file_path}")
    await _require_audio_decode_budget(req.file_path)

    cache_key = await run_blocking(_analysis_cache_key, req.file_path, req.mode)
    cached = await run_io_blocking(_ext_get, cache_key)
    if cached is not None:
        return ExtendedAnalysisResponse(**cached)
    base = await run_io_blocking(_base_get, cache_key)

    try:
        data = await run_blocking(compute_extended_result, req.file_path, req.mode, base,
                                  None, cache_key)
        return ExtendedAnalysisResponse(**data)
    except Exception as exc:
        _log.exception("Extended analyzer failed for %s", req.file_path)
        internal_error(cause=exc)


def _extract_mix_metrics(file_path: str) -> dict:
    import numpy as np

    y_stereo, sr = _load_audio(file_path, sr=None, mono=False)
    # Loudness on the real layout: mono is ONE BS.1770 channel (duplicating
    # it to stereo reads +3 LU hot). Shared meter; -70 floor for silence.
    from khensu_services.analysis.dsp_metrics import spectral_tilt_db_per_octave
    from khensu_services.analysis.loudness import integrated_lufs, loudness_range
    lufs = integrated_lufs(y_stereo, int(sr))
    lufs = float(lufs) if lufs is not None else -70.0
    lra = loudness_range(y_stereo, int(sr))
    lra = float(lra) if lra is not None else 0.0

    if y_stereo.ndim == 1:
        y_stereo = np.stack([y_stereo, y_stereo])
    y_mono = y_stereo.mean(axis=0)
    tilt_db_per_octave = spectral_tilt_db_per_octave(y_mono, int(sr))

    # Per-channel crest, max over channels - the shared convention (DSP-11).
    from khensu_services.analysis.loudness import crest_factor_db
    _crest = crest_factor_db(y_stereo)
    crest_db = float(_crest) if _crest is not None else 0.0

    centroid = _lean.spectral_centroid(y_mono, sr)
    centroid_hz = float(np.mean(centroid))

    from khensu_services.analysis.dsp_metrics import stereo_correlation
    correlation = stereo_correlation(y_stereo[0], y_stereo[1])

    fft_mag = np.abs(np.fft.rfft(y_mono))
    freqs = np.fft.rfftfreq(len(y_mono), d=1.0 / sr)
    energy_total = float(np.sum(fft_mag ** 2) + 1e-12)
    energy_low = float(np.sum(fft_mag[freqs < 250] ** 2))
    balance_low = energy_low / energy_total

    _OCTAVE_HZ = [31.5, 63.0, 125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0, 16000.0]
    _SQRT2 = 1.41421356
    rms_per_octave_db = []
    for fc in _OCTAVE_HZ:
        mask = (freqs >= fc / _SQRT2) & (freqs < fc * _SQRT2)
        band_frac = float(np.sum(fft_mag[mask] ** 2)) / energy_total
        rms_per_octave_db.append(10.0 * np.log10(max(band_frac, 1e-12)))

    return {
        "lufs": lufs, "lra": lra, "crest_db": crest_db,
        "centroid_hz": centroid_hz, "correlation": correlation, "balance_low": balance_low,
        "rms_per_octave_db": rms_per_octave_db,
        "tilt_db_per_octave": tilt_db_per_octave,
    }


_REFERENCE_OCTAVE_HZ = [31.5, 63.0, 125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0, 16000.0]


def _octave_rms_db_for_segment(y_mono, sr: float) -> list[float]:
    """Shared octave-band RMS computation, factored out of _extract_mix_metrics
    so the whole-file and windowed (dynamic reference match) paths use
    identical band math."""
    import numpy as np
    _SQRT2 = 1.41421356
    fft_mag = np.abs(np.fft.rfft(y_mono))
    freqs = np.fft.rfftfreq(len(y_mono), d=1.0 / sr)
    energy_total = float(np.sum(fft_mag ** 2) + 1e-12)
    out = []
    for fc in _REFERENCE_OCTAVE_HZ:
        mask = (freqs >= fc / _SQRT2) & (freqs < fc * _SQRT2)
        band_frac = float(np.sum(fft_mag[mask] ** 2)) / energy_total
        out.append(10.0 * np.log10(max(band_frac, 1e-12)))
    return out


def _compute_windowed_metrics(file_path: str, n_windows: int = 8) -> list[dict]:
    """Splits the track into n_windows equal time segments and computes
    per-segment centroid/crest/octave-band metrics.

    Unlike _extract_mix_metrics's single whole-file average, this lets a
    reference comparison show *where* a mix drifts from its reference (e.g.
    "your chorus reads brighter than the reference's chorus") instead of one
    track-wide number that can hide section-to-section problems.
    Returns [] for tracks too short to window meaningfully (<1s per window).
    """
    import numpy as np
    import librosa
    from khensu_services.analysis.time_windows import iter_time_windows

    y_stereo, sr = _load_audio(file_path, sr=None, mono=False)
    if y_stereo.ndim == 1:
        y_stereo = np.stack([y_stereo, y_stereo])
    y_mono = y_stereo.mean(axis=0)
    sr = float(sr)

    from khensu_services.analysis.loudness import crest_factor_db
    from khensu_services.analysis.time_windows import time_window_bounds
    bounds = time_window_bounds(y_mono.shape[0], sr, n_windows)

    windows = []
    for _i, _frac, start_sec, end_sec, seg in iter_time_windows(y_mono, sr, n_windows):
        # Per-channel crest over the same window, max over channels (DSP-11).
        _s0, _s1 = bounds[_i]
        _crest = crest_factor_db(y_stereo[:, _s0:_s1])
        crest_db = float(_crest) if _crest is not None else 0.0
        centroid = librosa.feature.spectral_centroid(y=seg, sr=sr)
        centroid_hz = float(np.mean(centroid)) if centroid.size else 0.0

        windows.append({
            "start_sec": start_sec,
            "end_sec": end_sec,
            "centroid_hz": centroid_hz,
            "crest_db": crest_db,
            "octave_rms_db": _octave_rms_db_for_segment(seg, sr),
        })
    return windows


@router.post("/analyze/sections", response_model=SectionsAnalysisResponse, response_model_exclude_unset=True)
async def analyze_with_sections(req: SectionsAnalysisRequest) -> SectionsAnalysisResponse:
    import pathlib
    import khensu_services.analysis.router as main_router

    if not pathlib.Path(req.file_path).exists():
        not_found(f"File not found: {req.file_path}")
    await _require_audio_decode_budget(req.file_path)

    try:
        def _compute() -> dict:
            # The quick pass is shared with /analyze and full intake
            # (PERF-2); the section pass itself is cached below (CE-PLT-2).
            base = cached_analyze_quick(req.file_path)

            # Mono stays 1-D: section LUFS is BS.1770 on the real layout
            # (duplicating mono to stereo read every section +3 LU hot).
            y_stereo, sr = _load_audio(req.file_path, sr=None, mono=False)

            total_duration = y_stereo.shape[-1] / sr
            raw_sections   = base.get("sections", [{"start_sec": 0.0, "label": "A"}])
            section_data   = main_router.analyze_sections(y_stereo, int(sr), raw_sections, total_duration)

            section_results = [SectionAnalysisResult(**s) for s in section_data]
            loudness_arc    = compute_loudness_arc(section_data)

            return SectionsAnalysisResponse(
                **base,
                section_analysis=section_results,
                loudness_arc=LoudnessArcResult(**loudness_arc),
            ).model_dump(mode="json", exclude_unset=True)

        def _run():
            data = _acache.persistent_call(req.file_path, "analyze_sections", None, _compute)
            return SectionsAnalysisResponse(**data)

        return await run_blocking(_run)
    except HTTPException:
        raise
    except Exception as exc:
        _log.exception("Sections analyzer failed for %s", req.file_path)
        internal_error(cause=exc)


def _cached_mix_metrics(file_path: str) -> dict:
    return _acache.persistent_call(file_path, "mix_metrics", None,
                                   _extract_mix_metrics, file_path)


def _cached_windowed_metrics(file_path: str, n_windows: int = 8) -> list[dict]:
    return _acache.persistent_call(file_path, "windowed_metrics", {"n_windows": n_windows},
                                   _compute_windowed_metrics, file_path, n_windows)


@router.post("/analyze/reference", response_model=ReferenceAnalysisResponse, response_model_exclude_unset=True)
async def analyze_reference(req: ReferenceAnalysisRequest) -> ReferenceAnalysisResponse:
    import pathlib

    for path in (req.target_path, req.reference_path):
        if not pathlib.Path(path).exists():
            not_found(f"File not found: {path}")
    await _require_audio_decode_budget(req.target_path, req.reference_path)

    try:
        # Per-file and persistent (CE-PLT-2): a reference track compared
        # against several mixes is measured once, not 2 decodes per call.
        target = await run_blocking(_cached_mix_metrics, req.target_path)
        ref = await run_blocking(_cached_mix_metrics, req.reference_path)
        target_windows = await run_blocking(_cached_windowed_metrics, req.target_path)
        ref_windows = await run_blocking(_cached_windowed_metrics, req.reference_path)
    except Exception as exc:
        _log.exception("Reference analyzer failed")
        internal_error(cause=exc)

    # Pair windows by index (i.e. by song-progress fraction, since the two
    # tracks can have different lengths) — dropping any trailing unmatched
    # windows if one track produced fewer than the other.
    time_windows = [
        ReferenceTimeWindow(
            window_index=i,
            target_start_sec=tw["start_sec"], target_end_sec=tw["end_sec"],
            reference_start_sec=rw["start_sec"], reference_end_sec=rw["end_sec"],
            target_centroid_hz=tw["centroid_hz"], reference_centroid_hz=rw["centroid_hz"],
            delta_centroid_hz=tw["centroid_hz"] - rw["centroid_hz"],
            target_crest_db=tw["crest_db"], reference_crest_db=rw["crest_db"],
            delta_crest_db=tw["crest_db"] - rw["crest_db"],
            delta_octave_db=[t - r for t, r in zip(tw["octave_rms_db"], rw["octave_rms_db"])],
        )
        for i, (tw, rw) in enumerate(zip(target_windows, ref_windows))
    ]

    t_oct = target["rms_per_octave_db"]
    r_oct = ref["rms_per_octave_db"]
    _OCTAVE_HZ = [31.5, 63.0, 125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0, 16000.0]

    # Corrective match-EQ (CON-9): the reference chain's own bands, so the
    # Compare card's match curve and the chain that renders it are one answer
    # (this used to be a separate per-octave ref - target, clamped to +/-6 dB).
    from khensu_services.copilot.reference_chain import match_eq_bands
    eq, _before, _after, _edges = await run_blocking(match_eq_bands, req.target_path, req.reference_path)
    match_centres = [float(e["parameters"]["cutoff_frequency_hz"]) for e in eq]
    match_gains = [float(e["parameters"]["gain_db"]) for e in eq]

    delta_lra      = target["lra"] - ref["lra"]
    delta_crest_db = target["crest_db"] - ref["crest_db"]

    profile_recommendations: list[str] = []
    if abs(delta_lra) > 3.0:
        if delta_lra > 0:
            profile_recommendations.append(
                f"Your mix is {delta_lra:.1f} LU more dynamic than the reference (LRA: "
                f"{target['lra']:.1f} LU vs {ref['lra']:.1f} LU). "
                "Add gentle bus compression (2:1–3:1) to tighten the dynamic range to match."
            )
        else:
            profile_recommendations.append(
                f"Your mix is {abs(delta_lra):.1f} LU more compressed than the reference (LRA: "
                f"{target['lra']:.1f} LU vs {ref['lra']:.1f} LU). "
                "Reduce bus compression ratio or increase attack time to open up dynamics."
            )
    if abs(delta_crest_db) > 4.0:
        if delta_crest_db > 0:
            profile_recommendations.append(
                f"Your mix has {delta_crest_db:.1f} dB more transient headroom than the reference "
                f"(crest: {target['crest_db']:.1f} dB vs {ref['crest_db']:.1f} dB). "
                "Consider light limiting or parallel compression to increase RMS density."
            )
        else:
            profile_recommendations.append(
                f"Your mix is {abs(delta_crest_db):.1f} dB more limited/compressed than the reference "
                f"(crest: {target['crest_db']:.1f} dB vs {ref['crest_db']:.1f} dB). "
                "Reduce limiter gain reduction to recover transient definition."
            )

    return ReferenceAnalysisResponse(
        target_lufs=target["lufs"],           reference_lufs=ref["lufs"],
        delta_lufs=target["lufs"] - ref["lufs"],
        target_lra=target["lra"],             reference_lra=ref["lra"],
        delta_lra=delta_lra,
        target_crest_db=target["crest_db"],   reference_crest_db=ref["crest_db"],
        delta_crest_db=delta_crest_db,
        target_centroid_hz=target["centroid_hz"], reference_centroid_hz=ref["centroid_hz"],
        delta_centroid_hz=target["centroid_hz"] - ref["centroid_hz"],
        target_stereo_correlation=target["correlation"], reference_stereo_correlation=ref["correlation"],
        delta_stereo_correlation=target["correlation"] - ref["correlation"],
        target_balance_low=target["balance_low"], reference_balance_low=ref["balance_low"],
        delta_balance_low=target["balance_low"] - ref["balance_low"],
        octave_center_hz=_OCTAVE_HZ,
        target_octave_rms_db=t_oct,
        reference_octave_rms_db=r_oct,
        delta_octave_db=[t - r for t, r in zip(t_oct, r_oct)],
        match_eq_center_hz=match_centres,
        match_eq_gain_db=match_gains,
        profile_recommendations=profile_recommendations,
        time_windows=time_windows,
    )


@router.post("/analyze/drums", response_model=DrumAnalysisResponse, response_model_exclude_unset=True)
async def analyze_drums_endpoint(req: DrumAnalysisRequest) -> DrumAnalysisResponse:
    import pathlib
    import numpy as np
    from khensu_services.analysis.drum_analyzer import analyze_drums

    if not pathlib.Path(req.file_path).exists():
        not_found(f"File not found: {req.file_path}")
    await _require_audio_decode_budget(req.file_path)

    try:
        def _measure() -> dict:
            y, sr = _load_audio(req.file_path, sr=None, mono=False)
            if y.ndim == 1:
                y = np.stack([y, y])
            return analyze_drums(y, int(sr))

        def _run() -> DrumAnalysisResponse:
            result = _acache.persistent_call(req.file_path, "analyze_drums", None, _measure)

            def _part(d: dict) -> DrumPartResult:
                return DrumPartResult(
                    count=d["count"],
                    avg_bpm=d["avg_bpm"],
                    timing_regularity=d["timing_regularity"],
                    avg_body_db=d["avg_body_db"],
                    avg_fund_hz=d["avg_fund_hz"],
                    events=[DrumEventResult(**e) for e in d.get("events", [])],
                )

            return DrumAnalysisResponse(
                kick=_part(result["kick"]),
                snare=_part(result["snare"]),
                transient_sharpness=result["transient_sharpness"],
                kick_bass_proximity_hz=result.get("kick_bass_proximity_hz", 0.0),
                kick_bass_frequency_clash=result.get("kick_bass_frequency_clash", False),
                bass_fundamental_hz=result.get("bass_fundamental_hz", 0.0),
            )

        return await run_blocking(_run)
    except Exception as exc:
        _log.exception("Drum analyzer failed for %s", req.file_path)
        internal_error(cause=exc)


@router.post("/analyze/integrity", response_model=FileIntegrityResult, response_model_exclude_unset=True)
async def check_integrity(req: FileIntegrityRequest) -> FileIntegrityResult:
    import pathlib
    from khensu_services.analysis.integrity_checker import check_file_integrity

    if not pathlib.Path(req.file_path).exists():
        not_found(f"File not found: {req.file_path}")
    # It reads the whole file: the same size limit as every other scan.
    await _require_audio_decode_budget(req.file_path)

    try:
        result = await run_blocking(check_file_integrity, req.file_path)
        return FileIntegrityResult(**result)
    except Exception as exc:
        if undecodable_audio_cause(exc) is not None:
            invalid_input(f"Could not decode {req.file_path}", cause=exc)
        _log.exception("Integrity check failed for %s", req.file_path)
        internal_error(cause=exc)


class AnalysisMetadataResponse(ResponseModel):
    service_version: str
    python_version: str
    librosa_version: str
    numpy_version: str
    scipy_version: str
    pyloudnorm_version: str
    soundfile_version: str
    analysis_schema_version: str
    genre_heuristic_version: str


@router.get("/analyze/metadata", response_model=AnalysisMetadataResponse,
            response_model_exclude_unset=True)
async def get_analysis_metadata() -> dict:
    import importlib.metadata

    def _ver(pkg: str) -> str:
        try:
            return importlib.metadata.version(pkg)
        except importlib.metadata.PackageNotFoundError:
            return "unknown"

    from khensu_services.analysis.genre_detector import GENRE_HEURISTIC_VERSION
    return {
        "service_version":          SERVICE_VERSION,
        "python_version":           __import__("sys").version.split()[0],
        "librosa_version":          _ver("librosa"),
        "numpy_version":            _ver("numpy"),
        "scipy_version":            _ver("scipy"),
        "pyloudnorm_version":       _ver("pyloudnorm"),
        "soundfile_version":        _ver("soundfile"),
        "analysis_schema_version":  "2026-06-28",
        "genre_heuristic_version":  GENRE_HEURISTIC_VERSION,
    }


class ExportTarget(ResponseModel):
    id: str
    name: str
    category: str
    lufs_integrated: float
    integrated_tolerance_lu: float | None = None
    true_peak_dbtp: float
    lra_max: float | None = None
    short_term_max_lufs: float | None = None
    measurement: str
    standard: str
    notes: str


class ExportTargetsResponse(ResponseModel):
    targets: list[ExportTarget]


@router.get("/analyze/export-targets", response_model=ExportTargetsResponse,
            response_model_exclude_unset=True)
async def get_export_targets() -> dict:
    """Every delivery loudness target (streaming, broadcast, podcast, club,
    physical) from the single table in ``analysis/delivery_targets.py``."""
    from khensu_services.analysis.delivery_targets import DELIVERY_TARGETS
    return {"targets": [dict(t) for t in DELIVERY_TARGETS]}


@router.post("/analyze/vocal", response_model=VocalAnalysisResponse, response_model_exclude_unset=True)
async def analyze_vocal_endpoint(req: VocalRequest) -> VocalAnalysisResponse:
    """Time-resolved vocal metrics: sibilance timeline, breath markers,
    presence/clarity/warmth tiles, and formant balance."""
    from khensu_services.analysis.vocal_analyzer import analyze_vocal

    if not os.path.isfile(req.file_path):
        not_found(f"File not found: {req.file_path}")
    await _require_audio_decode_budget(req.file_path)

    try:
        def _run():
            y, sr = _load_audio(req.file_path, sr=None, mono=False)
            return analyze_vocal(y, int(sr), vocals_path=req.file_path)
        result = await run_blocking(_run)
        return VocalAnalysisResponse(**result)
    except Exception as exc:
        _log.exception("Vocal analysis failed for %s", req.file_path)
        internal_error(cause=exc)


@router.post("/analyze/lyrics", response_model=LyricAnalysisResponse, response_model_exclude_unset=True)
async def analyze_lyrics(req: LyricsRequest) -> LyricAnalysisResponse:
    """Word-level transcription (faster-whisper) + per-word vocal-presence-band
    masking check — flags lyric moments likely to read as unintelligible.
    503 if the optional [asr] extra (faster-whisper) isn't installed."""
    from khensu_services.analysis.lyric_analyzer import (
        transcribe_lyrics, analyze_lyric_intelligibility, ASRNotAvailableError,
    )

    if not os.path.isfile(req.file_path):
        not_found(f"File not found: {req.file_path}")
    await _require_audio_decode_budget(req.file_path)

    try:
        def _measure():
            words = transcribe_lyrics(req.file_path, model_size=req.model_size)
            y, sr = _load_audio(req.file_path, sr=None, mono=True)
            return analyze_lyric_intelligibility(y, sr, words)

        def _run():
            # Transcription is the slowest scan there is and has two callers:
            # the memory tier answers the second, and two that arrive together
            # share one pass. Not on disk - see _lyrics_cache.
            ck = file_cache_key(req.file_path, "lyrics", req.model_size)
            if ck is None:
                return _measure()

            def _fill():
                data = _measure()
                _lyrics_cache[ck] = data
                return data

            return _lyrics_flights.do(ck, _fill, probe=lambda: _lyrics_cache.get(ck))
        result = await run_blocking(_run)
        return LyricAnalysisResponse(**result)
    except ASRNotAvailableError as exc:
        unavailable(str(exc), cause=exc)
    except Exception as exc:
        _log.exception("Lyric analysis failed for %s", req.file_path)
        internal_error(cause=exc)


@router.post("/analyze/chords", response_model=ChordsResponse, response_model_exclude_unset=True)
async def analyze_chords(req: ChordsRequest) -> ChordsResponse:

    if not os.path.isfile(req.file_path):
        not_found(f"File not found: {req.file_path}")
    await _require_audio_decode_budget(req.file_path)

    # Native-rate CQT is expensive: cache per file content stamp.
    ck = file_cache_key(req.file_path, "chords", 512)
    cached = _chords_cache.get(ck) if ck is not None else None
    if cached is not None:
        return ChordsResponse(**cached)
    # Disk tier (PERF-3): a reopened session or a restarted service reads
    # the stored chords instead of another native-rate CQT.
    # Hashing the file and reading / writing the entry are file I/O: off the
    # event loop (FIX-17).
    disk_key = await run_blocking(_acache.key_for_analyzer, req.file_path, "analyze_chords", 512)
    stored = await run_io_blocking(_acache.lookup, "analyze_chords", disk_key)
    if stored is not None:
        if ck is not None:
            _chords_cache[ck] = stored
        return ChordsResponse(**stored)

    try:
        y, sr = await run_blocking(_load_audio, req.file_path, sr=None, mono=True)
    except Exception as exc:
        _log.exception("Failed to load audio for chord analysis: %s", req.file_path)
        internal_error(cause=exc)

    hop_length = 512
    try:
        # safe_chroma_cqt, not raw chroma_cqt (audit AUD-13): the default
        # 7-octave C1 basis exceeds Nyquist under ~8.4 kHz, and this endpoint
        # used to answer HTTP 500 there where it can simply degrade - the
        # chord templates below are pure 12-bin pitch-class math.
        from khensu_services.analysis._key import safe_chroma_cqt
        chroma = await run_blocking(safe_chroma_cqt, y, sr, hop_length=hop_length)
    except Exception as exc:
        _log.exception("chroma_cqt failed for %s", req.file_path)
        # Generic detail (SVC-09): the exception text goes to the log only.
        internal_error(cause=exc)

    # Template matching (triads, 7ths, dim, sus), "N" for no-chord, Viterbi
    # smoothing and a Krumhansl-Schmuckler key - see detect_chord_sequence.
    # One segment per tracked beat (NEW-8); fixed half seconds when the
    # beats cannot be followed.
    from khensu_services.analysis.performance_analyzer import beat_segment_frames, detect_chord_sequence

    def _chords() -> dict:
        import librosa

        from khensu_services.analysis import _lean_spectral as _lean
        try:
            _tempo, beat_frames = _lean.beat_track(y=y, sr=int(sr), hop_length=hop_length)
            beat_times = librosa.frames_to_time(beat_frames, sr=int(sr), hop_length=hop_length).tolist()
        except Exception:
            beat_times = []
        starts = beat_segment_frames(beat_times, int(sr), hop_length, chroma.shape[1])
        return detect_chord_sequence(chroma, int(sr), hop_length, segment_starts=starts)

    result = await run_blocking(_chords)
    response = ChordsResponse(
        chords=[ChordEvent(**c) for c in result["chords"]],
        key=result["key"], mode=result["mode"], key_confidence=result["key_confidence"],
        camelot=result.get("camelot"),
        circle_of_fifths_index=result.get("circle_of_fifths_index"),
        relative_key=result.get("relative_key"), parallel_key=result.get("parallel_key"))
    dumped = response.model_dump(mode="json")
    if ck is not None:
        _chords_cache[ck] = dumped
    await run_io_blocking(_acache.store, "analyze_chords", disk_key, dumped)
    return response
