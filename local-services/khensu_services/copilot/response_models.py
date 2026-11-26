"""Typed response models for the /copilot job and utility routes.

Each model mirrors the dict its endpoint already returned (routes declare
``response_model_exclude_unset=True``, so the wire shape is unchanged). Keys
the desktop or plugin reads are spelled out so the generated OpenAPI schema,
and the C++ field constants generated from it, name them. Irregular nested
payloads (harmonic reports, stage levels, section reports) stay
``dict[str, Any]``; the guardrail and mastering-iteration payloads are typed
below so the C++ side reads their keys through generated constants.
"""
from __future__ import annotations

from typing import Any, Literal

from khensu_services.common.api_models import ResponseModel
from khensu_services.common.job_status import JobStatus
from khensu_services.copilot.mixer_chain_generator import MixerChainResponse

#: A JSON number. ``int | float`` (not ``float``) so an integer stays an
#: integer on the wire, exactly as the untyped dict serialized it.
Number = int | float


# --- generic job envelope ----------------------------------------------------

class JobStartResponse(ResponseModel):
    job_id: str


class CopilotJobCancelResponse(ResponseModel):
    """``ok`` is True only for a known job id (SVC-14); ``status`` is the job's
    resulting state (``cancelling``/``cancelled``/its terminal state, or
    ``unknown``)."""
    ok: bool
    status: str | None = None


class JobStatusBase(ResponseModel):
    status: JobStatus
    error: str | None = None


# --- safety guardrails ------------------------------------------------------------

class GuardrailMetrics(ResponseModel):
    """One side (before/after) of the post-render guardrail comparison.
    Empty when the guardrail could not run (``_guardrail_failed``)."""
    true_peak_dbtp: Number | None = None
    clip_samples: int | None = None
    mono_low_corr: Number | None = None
    crest_db: Number | None = None
    vocal_presence_proxy: Number | None = None


class PreRenderValidation(ResponseModel):
    """A pre-render refusal: ``validate_render_request`` (layout and effect
    checks), ``validate_stem_attribution``, or a genre-action block."""
    ok: bool
    blocks: list[str] | None = None
    warnings: list[str] | None = None
    channels: int | None = None
    channel_layout: str | None = None
    lfe_protected: bool | None = None
    sample_rate: int | None = None
    duration_sec: Number | None = None
    low_band_correlation: Number | None = None


class RenderGuardrail(ResponseModel):
    """``render_guardrails.post_render_guardrail`` (``ok`` .. ``artifact_score_delta``),
    or ``{"pre_render": ...}`` alone when the render was refused before it ran."""
    ok: bool | None = None
    matched_gain_db: Number | None = None
    before: GuardrailMetrics | None = None
    after: GuardrailMetrics | None = None
    blocks: list[str] | None = None
    warnings: list[str] | None = None
    artifact_score_delta: Number | None = None
    pre_render: PreRenderValidation | None = None


# --- render jobs (apply_chain, apply_section_chain) ----------------------------

class RenderJobResult(ResponseModel):
    """A render job's result: a promoted render, a pre-render block, or a
    post-render guardrail rejection (``status`` tells which)."""
    status: str | None = None                   # success | blocked | rejected
    output_path: str | None = None
    applied_effects: list[str] | None = None
    skipped_effects: list[str] | None = None
    harmonic_reports: list[dict[str, Any]] | None = None
    stage_levels: list[dict[str, Any]] | None = None
    guardrail: RenderGuardrail | None = None
    decision_hash: str | None = None
    # Section-aware renders: one report per section.
    sections: list[dict[str, Any]] | None = None
    # Guardrail rejection (the expert-override inputs).
    guardrail_status: str | None = None
    render_state: str | None = None
    quarantine_path: str | None = None
    measurements: RenderGuardrail | None = None
    source_path: str | None = None
    # Non-fatal render notes, e.g. a key stem that could not be used (ENH-6).
    warnings: list[str] | None = None


class RenderJobStatusResponse(JobStatusBase):
    result: RenderJobResult | None = None


# --- batch apply_chain -----------------------------------------------------------

class BatchJobStartResponse(JobStartResponse):
    total_files: int


class BatchProgress(ResponseModel):
    total: int
    done: int
    failed: int


class BatchFileResult(ResponseModel):
    file_path: str
    output_path: str
    # success | blocked (pre-render validator / stem attribution) |
    # rejected (post-render guardrail; the render stays in quarantine) | failed
    status: str | None = None
    # validated | blocked | rejected | unvalidated - the same terminal render
    # state vocabulary as the single-file routes (CPL-01).
    render_state: str | None = None
    applied_effects: list[str] | None = None
    skipped_effects: list[str] | None = None
    error: str | None = None
    # The post-render guardrail verdict (or {"pre_render": ...} when blocked).
    guardrail: RenderGuardrail | None = None
    # A rejected/unvalidated render's quarantine file, for the expert override.
    quarantine_path: str | None = None
    stem_provenance: dict[str, Any] | None = None
    stem_paths: dict[str, str] | None = None
    # Non-fatal render notes, e.g. a key stem that could not be used (ENH-6).
    warnings: list[str] | None = None


class BatchJobStatusResponse(JobStatusBase):
    progress: BatchProgress
    results: list[BatchFileResult]
    stem_provenance: dict[str, Any] | None = None
    file_stem_paths: dict[str, Any] | None = None
    decision_hash: str | None = None


# --- mastering agent -------------------------------------------------------------

class MasteringChainStage(ResponseModel):
    effect: str
    parameters: dict[str, Any]


class MasteringMixMetrics(ResponseModel):
    """``router_audio._extract_mix_metrics`` plus the shared crest."""
    lufs: Number
    lra: Number | None = None
    crest_db: Number
    centroid_hz: Number
    correlation: Number | None = None
    balance_low: Number | None = None
    rms_per_octave_db: list[Number] | None = None
    tilt_db_per_octave: Number | None = None


class MasteringMetricDelta(ResponseModel):
    measured: Number
    target: Number
    delta: Number


class MasteringDeltas(ResponseModel):
    lufs: MasteringMetricDelta
    crest_db: MasteringMetricDelta
    centroid_hz: MasteringMetricDelta


class MasteringIteration(ResponseModel):
    """One render -> analyze -> critique pass of ``run_mastering_agent``."""
    iteration: int
    chain: list[MasteringChainStage]
    metrics: MasteringMixMetrics
    deltas: MasteringDeltas
    objective_score: Number
    vocal_presence_delta: Number | None = None
    artifact_score_delta: Number | None = None
    converged: bool
    safe: bool
    objective_eligible: bool
    # None only for the untouched-source measurement pass.
    guardrail: RenderGuardrail | None = None
    output_path: str
    critique: str | None = None


class MasteringAgentTargets(ResponseModel):
    lufs: Number
    crest_db: Number
    centroid_hz: Number
    ceiling_dbtp: Number


class MasteringAgentResult(ResponseModel):
    converged: bool
    stop_reason: str | None = None
    iterations: list[MasteringIteration]
    requires_user_choice: bool
    best_safe_output_path: str | None = None
    best_safe_iteration: int | None = None
    final_output_path: str | None = None
    decision_hash: str | None = None
    guardrail: RenderGuardrail | None = None
    # ENH-5: the style the run used and the targets it converged toward.
    style: str | None = None
    strength: Number | None = None
    style_summary: str | None = None
    targets: MasteringAgentTargets | None = None


class MasteringAgentStatusResponse(JobStatusBase):
    result: MasteringAgentResult | None = None


# --- platform masters ------------------------------------------------------------

class PlatformMastersStartResponse(JobStartResponse):
    total: int
    output_dir: str


class PlatformMastersProgress(ResponseModel):
    total: int
    done: int
    current: str | None = None


class PlatformMasterEntry(ResponseModel):
    platform_id: str
    platform_name: str
    # completed | could_not_reach | rejected (guardrail failed; the master
    # stays in quarantine, CPL-02) | failed
    status: str
    render_state: str | None = None             # validated | rejected | unvalidated
    error: str | None = None
    output_path: str | None = None
    quarantine_path: str | None = None
    gain_db: Number | None = None
    # Peak true-peak-limiter gain reduction the master needed (dB, >= 0).
    max_gain_reduction_db: Number | None = None
    achieved_lufs: Number | None = None
    achieved_true_peak_dbtp: Number | None = None
    target_lufs: Number | None = None
    ceiling_dbtp: Number | None = None
    loudness_miss_lu: Number | None = None
    target_reached: bool | None = None
    guardrail: RenderGuardrail | None = None


class PlatformMastersStatusResponse(JobStatusBase):
    progress: PlatformMastersProgress
    results: list[PlatformMasterEntry]
    output_dir: str


# --- chat / mixer chain jobs -------------------------------------------------------

class MixerChainJobStatusResponse(JobStatusBase):
    result: MixerChainResponse | None = None


# --- model downloads -------------------------------------------------------------

class ModelStatusResponse(ResponseModel):
    status: str
    installed: bool
    downloaded_bytes: Number
    total_bytes: Number
    progress: Number
    speed_kbps: Number
    eta_seconds: Number | None = None
    error_message: str | None = None
    models_dir: str | None = None
    demucs_package_available: bool


class SuccessResponse(ResponseModel):
    status: Literal["success"]


class SetModelsDirResponse(SuccessResponse):
    models_dir: str


# --- render utilities ------------------------------------------------------------

class OverrideGuardrailResponse(SuccessResponse):
    output_path: str
    measurements: RenderGuardrail


class AbComparison(ResponseModel):
    dimension: str
    metric: str
    a_value: Number | None = None
    b_value: Number | None = None
    winner: str


class AbCritiqueResponse(ResponseModel):
    label_a: str
    label_b: str
    loudness_match_gain_db: Number | None = None
    metrics_a: dict[str, Any]
    metrics_b: dict[str, Any]
    comparisons: list[AbComparison]
    overall: str
    verdict: str


class CrossfeedAbResponse(ResponseModel):
    headphone_path: str
    speaker_path: str
    width_headphone: Number | None = None
    width_speaker: Number | None = None
    width_reduction_pct: Number | None = None
    correlation_headphone: Number | None = None
    correlation_speaker: Number | None = None
    # Gain applied to the speaker render to match the headphone render's
    # integrated loudness (CPL-13); both files then share any peak trim.
    speaker_match_gain_db: Number | None = None
    note: str


class ReferenceChainMeasured(ResponseModel):
    target_lufs: Number | None = None
    reference_lufs: Number | None = None
    target_crest_db: Number | None = None
    reference_crest_db: Number | None = None
    target_width: Number | None = None
    reference_width: Number | None = None


class ReferenceChainCurve(ResponseModel):
    """FEAT-3: the tonal delta to the reference per band (+ = hotter than the
    reference) before the match-EQ and as the chain is predicted to leave it."""
    band_centers_hz: list[float] = []
    delta_before_db: list[float] = []
    delta_after_db: list[float] = []


class ReferenceChainResponse(ResponseModel):
    target_path: str
    reference_path: str
    amount: float = 1.0
    predicted_curve: ReferenceChainCurve | None = None
    effects: list[dict[str, Any]]
    rationale: list[str]
    pre_render: dict[str, Any] | None = None
    measured: ReferenceChainMeasured
