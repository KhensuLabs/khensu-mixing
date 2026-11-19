"""The file-analysis routes, as one table (ENG-5).

Each row was its own ``router_<name>.py`` doing the same require-file / cache /
disk-tier / DSP-pool dance by hand; ``common/file_analysis.register_file_analysis``
is that dance once. A route whose handler does more than run one analyzer over
its input files (jobs, uploads, several steps) keeps its own router.

``persist=True`` puts the result on the content-hash disk tier
(``common/analysis_cache``): only for results that depend on nothing but the
file bytes and the parameters (not on installed models or user data).
"""
from __future__ import annotations

from typing import Any

from fastapi import APIRouter

from khensu_services.analysis.analyzer_responses import (
    MeasurementQualityResponse, MixQualityResponse, QuickStatsResponse, SignalIntegrityResponse,
)
from khensu_services.analysis.models import (
    ArrangementRequest, ArrangementResponse, DepthRequest, DepthResponse, GainXrayRequest,
    GainXrayResponse, LoudnessCurveRequest, LoudnessCurveResponse, MeasurementQualityRequest,
    MidSideSpectrogramRequest, MidSideSpectrogramResponse, MixQualityRequest, MonoCompatRequest,
    MonoCompatResponse, PanBalanceRequest, PanBalanceResponse, PhaseHeatmapRequest,
    PhaseHeatmapResponse, QuickStatsRequest, ReferenceSpectralDeltaRequest,
    ReferenceSpectralDeltaResponse, SignalIntegrityRequest, SourceForensicsRequest,
    SourceForensicsResponse, SourceTriageRequest, SourceTriageResponse, SpectrotemporalRequest,
    SpectrotemporalResponse, StemEnergyTimelineRequest, StemEnergyTimelineResponse,
    StreamingNormalizationRequest, StreamingNormalizationResponse, SubBassPhaseRequest,
    SubBassPhaseResponse, TuningReferenceRequest, TuningReferenceResponse,
)
from khensu_services.common.errors import invalid_input
from khensu_services.common.file_analysis import lazy, register_file_analysis

router = APIRouter()


def _with_ctx(module: str, func: str):
    """Analyzers that take ``ctx=`` decode through the shared cache (PERF-2)."""
    fn = lazy(module, func)

    def call(req: Any) -> dict[str, Any]:
        from khensu_services.analysis.context import AnalysisContext
        return fn(req.file_path, ctx=AnalysisContext(req.file_path))
    return call


def _on_path(module: str, func: str):
    fn = lazy(module, func)
    return lambda req: fn(req.file_path)


def _grid(req: Any) -> list[int]:
    return [req.n_bands, req.target_frames]


def _loudness_curve(req: LoudnessCurveRequest) -> dict[str, Any]:
    from khensu_services.analysis.context import AnalysisContext
    from khensu_services.analysis.loudness import short_term_loudness_curve
    # Decode through the shared cache (the Dashboard opens several views of
    # the same file at once).
    y_stereo, sr = AnalysisContext(req.file_path).load(sr=None, mono=False)
    return short_term_loudness_curve(y_stereo, int(sr))


def _require_stems(req: DepthRequest) -> None:
    if not req.stems:
        invalid_input("No stems provided")


def _stem_energy_paths(req: StemEnergyTimelineRequest) -> dict[str, str]:
    stems = {"vocals": req.vocals_path, "drums": req.drums_path,
             "bass": req.bass_path, "other": req.other_path}
    return {k: v for k, v in stems.items() if v}


def _require_any_stem(req: StemEnergyTimelineRequest) -> None:
    if not _stem_energy_paths(req):
        invalid_input("At least one stem path is required")


def _spectrotemporal_range(req: SpectrotemporalRequest) -> None:
    if not (1 <= req.n_bands <= 128) or not (1 <= req.target_frames <= 2000):
        invalid_input("n_bands/target_frames out of range")


def _arrangement_paths(req: ArrangementRequest) -> list[tuple[str, str]]:
    return [("File", p) for p in (req.vocals_path, req.drums_path, req.bass_path, req.other_path)]


# --- Single-file scans on the disk tier -------------------------------------
register_file_analysis(router, "signal_integrity", "/analyze/signal_integrity",
                       SignalIntegrityRequest, SignalIntegrityResponse,
                       _on_path("signal_integrity", "analyze_signal_integrity"), persist=True)
register_file_analysis(router, "mono_compat", "/analyze/mono_compat",
                       MonoCompatRequest, MonoCompatResponse,
                       _on_path("mono_compat", "analyze_mono_compat"), persist=True)
register_file_analysis(router, "source_forensics", "/analyze/source_forensics",
                       SourceForensicsRequest, SourceForensicsResponse,
                       _on_path("source_forensics", "analyze_source_forensics"), persist=True)
register_file_analysis(router, "tuning_reference", "/analyze/tuning_reference",
                       TuningReferenceRequest, TuningReferenceResponse,
                       _on_path("tuning_reference", "analyze_tuning_reference"), persist=True)
register_file_analysis(router, "streaming_normalization", "/analyze/streaming_normalization",
                       StreamingNormalizationRequest, StreamingNormalizationResponse,
                       _on_path("streaming_normalization", "analyze_streaming_normalization"),
                       persist=True)
def _source_triage(req: SourceTriageRequest) -> dict[str, Any]:
    return lazy("source_triage", "triage_source")(req.file_path, material=req.material)


register_file_analysis(router, "source_triage", "/analyze/source_triage",
                       SourceTriageRequest, SourceTriageResponse, _source_triage,
                       params=lambda req: lazy("source_triage", "triage_cache_params")(req.material),
                       persist=True)
register_file_analysis(router, "measurement_quality", "/analyze/measurement_quality",
                       MeasurementQualityRequest, MeasurementQualityResponse,
                       _with_ctx("measurement_quality", "analyze_measurement_quality"), persist=True)
register_file_analysis(router, "quick_stats", "/analyze/quick_stats",
                       QuickStatsRequest, QuickStatsResponse,
                       _with_ctx("quick_stats", "analyze_quick_stats"), persist=True)
# Key and compute on the same 1 ms-rounded window (CLN-7): two windows that
# share a cache entry must produce the same result.
register_file_analysis(router, "pan_balance", "/analyze/pan_balance",
                       PanBalanceRequest, PanBalanceResponse,
                       lambda req: lazy("pan_balance", "analyze_pan_balance")(
                           req.file_path, round(req.window_sec, 3)),
                       params=lambda req: {"window_sec": round(req.window_sec, 3)}, persist=True)
register_file_analysis(router, "phase_heatmap", "/analyze/phase_heatmap",
                       PhaseHeatmapRequest, PhaseHeatmapResponse,
                       lambda req: lazy("phase_heatmap", "analyze_phase_heatmap")(
                           req.file_path, req.n_bands, req.target_frames),
                       params=_grid, persist=True)
register_file_analysis(router, "mid_side_spectrogram", "/analyze/mid_side_spectrogram",
                       MidSideSpectrogramRequest, MidSideSpectrogramResponse,
                       lambda req: lazy("mid_side_spectrogram", "analyze_mid_side_spectrogram")(
                           req.file_path, req.n_bands, req.target_frames),
                       params=_grid, persist=True)
register_file_analysis(router, "reference_spectral_delta", "/analyze/reference_spectral_delta",
                       ReferenceSpectralDeltaRequest, ReferenceSpectralDeltaResponse,
                       lambda req: lazy("reference_spectral_delta", "analyze_reference_spectral_delta")(
                           req.target_path, req.reference_path, req.n_bands, req.target_frames),
                       files=lambda req: [("target", req.target_path), ("reference", req.reference_path)],
                       params=_grid, persist=True)

# --- Memory cache only -------------------------------------------------------
register_file_analysis(router, "mix_quality", "/analyze/mix_quality",
                       MixQualityRequest, MixQualityResponse,
                       _on_path("mix_quality", "analyze_mix_quality"))
register_file_analysis(router, "loudness_curve", "/analyze/loudness_curve",
                       LoudnessCurveRequest, LoudnessCurveResponse, _loudness_curve)
register_file_analysis(router, "spectrotemporal", "/analyze/spectrotemporal",
                       SpectrotemporalRequest, SpectrotemporalResponse,
                       lambda req: lazy("spectrotemporal", "compute_spectrotemporal_frames")(
                           req.file_path, req.n_bands, req.target_frames),
                       params=_grid, validate=_spectrotemporal_range)
register_file_analysis(router, "gain_xray", "/analyze/gain_xray",
                       GainXrayRequest, GainXrayResponse,
                       lambda req: lazy("gain_xray", "analyze_gain_xray")(
                           req.original_path, req.processed_path),
                       files=lambda req: [("File", req.original_path), ("File", req.processed_path)])
register_file_analysis(router, "sub_bass_phase", "/analyze/sub_bass_phase",
                       SubBassPhaseRequest, SubBassPhaseResponse,
                       lambda req: lazy("sub_bass_phase", "analyze_sub_bass_phase")(
                           req.kick_path, req.bass_path, req.cutoff_hz),
                       files=lambda req: [("kick stem", req.kick_path), ("bass stem", req.bass_path)],
                       params=lambda req: req.cutoff_hz)
register_file_analysis(router, "depth", "/analyze/depth", DepthRequest, DepthResponse,
                       lambda req: lazy("depth_mapper", "analyze_depth")(req.stems),
                       files=lambda req: {f"File for '{k}'": v for k, v in req.stems.items()},
                       validate=_require_stems)
register_file_analysis(router, "arrangement", "/analyze/arrangement",
                       ArrangementRequest, ArrangementResponse,
                       lambda req: lazy("arrangement_analyzer", "analyze_arrangement")(
                           req.vocals_path, req.drums_path, req.bass_path, req.other_path,
                           req.window_sec),
                       files=_arrangement_paths, params=lambda req: round(req.window_sec, 3))
register_file_analysis(router, "stem_energy_timeline", "/analyze/stem_energy_timeline",
                       StemEnergyTimelineRequest, StemEnergyTimelineResponse,
                       lambda req: lazy("stem_energy_timeline", "compute_stem_energy_timeline")(
                           _stem_energy_paths(req)),
                       files=lambda req: {f"{k} stem": v for k, v in _stem_energy_paths(req).items()},
                       validate=_require_any_stem)
