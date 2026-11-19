"""Response models for the analyzer / humanize routes (typed-client contract).

Each model declares every key its endpoint returns, across all branches
(degraded, unavailable, empty-input), so the OpenAPI schema names every field
the desktop and plugin read. Routes use ``response_model_exclude_unset=True``,
so the wire shape stays exactly the dict the analyzer built.

Every field is optional on purpose: these are descriptions of dicts built by
analyzers with several early-return branches, and a "required" field that one
rare branch omits would turn into a production 500. Numbers that are not
always integers are ``float``; integer counts / MIDI numbers are ``int``.
Maps whose keys are data (per-service likelihoods, per-model entries) are
``dict[str, ...]``.
"""
from __future__ import annotations

from typing import Any

from pydantic import Field

from khensu_services.common.api_models import ResponseModel

# --- /analyze/metric_timeline -------------------------------------------------


class MetricTimelineWindow(ResponseModel):
    start_sec: float | None = None
    end_sec: float | None = None
    loudness_lufs: float | None = None
    crest_db: float | None = None
    harshness: float | None = None


class MetricTimelineHotspot(ResponseModel):
    window_index: int | None = None
    value: float | None = None


class MetricTimelineHotspots(ResponseModel):
    loudest: MetricTimelineHotspot | None = None
    most_squashed: MetricTimelineHotspot | None = None
    harshest: MetricTimelineHotspot | None = None


class MetricTimelineResponse(ResponseModel):
    block_sec: float | None = None
    hop_sec: float | None = None
    duration_sec: float | None = None
    n_windows: int | None = None
    windows: list[MetricTimelineWindow] | None = None
    hotspots: MetricTimelineHotspots | None = None
    # CE-PLT-2 incremental re-measure (only with baseline_path).
    recomputed_windows: int | None = None
    reused_windows: int | None = None


# --- /analyze/mix_quality -----------------------------------------------------


class MixQualityReliabilityBin(ResponseModel):
    bin_lo: float | None = None
    bin_hi: float | None = None
    mean_confidence: float | None = None
    observed_accuracy: float | None = None
    count: int | None = None


class MixQualityResponse(ResponseModel):
    available: bool | None = None
    reason: str | None = None
    quality_band: str | None = None
    quality_probability: float | None = None
    number_withheld_reason: str | None = None
    intervention_validated: bool | None = None
    defect_kinds: list[Any] | None = None
    calibration_ece: float | None = None
    calibration_ece_limit: float | None = None
    reliability_diagram: list[MixQualityReliabilityBin] | None = None


# --- /analyze/reference_match -------------------------------------------------


class ReferenceProfile(ResponseModel):
    lufs_integrated: float | None = None
    lra: float | None = None
    crest_factor_db: float | None = None
    spectral_tilt_db_per_octave: float | None = None


class ReferenceMatchEntry(ResponseModel):
    reference_index: int | None = None
    genre_id: str | None = None
    distance: float | None = None
    reference: ReferenceProfile | None = None
    delta: ReferenceProfile | None = None


class ReferenceMatchSection(ResponseModel):
    label: str | None = None
    start_sec: float | None = None
    end_sec: float | None = None
    duration_sec: float | None = None
    target_profile: ReferenceProfile | None = None
    matches: list[ReferenceMatchEntry] | None = None
    nearest: ReferenceMatchEntry | None = None
    guidance: str | None = None


class ReferenceMatchResponse(ResponseModel):
    target_profile: ReferenceProfile | None = None
    matches: list[ReferenceMatchEntry] | None = None
    nearest: ReferenceMatchEntry | None = None
    genre_restriction: str | None = None
    is_bootstrap_data: bool | None = None
    guidance: str | None = None
    sections: list[ReferenceMatchSection] | None = None


# --- /analyze/evidence_graph --------------------------------------------------


class EvidenceNode(ResponseModel):
    id: str | None = None
    kind: str | None = None
    label: str | None = None
    severity: float | None = None
    source: str | None = None
    band_hz: list[float] | None = None


class EvidenceEdge(ResponseModel):
    source: str | None = None
    target: str | None = None
    relation: str | None = None
    rationale: str | None = None


class EvidenceGraphResponse(ResponseModel):
    nodes: list[EvidenceNode] | None = None
    edges: list[EvidenceEdge] | None = None
    chains: list[str] | None = None


# --- /analyze/measurement_quality ---------------------------------------------


class QualifiedMeasurement(ResponseModel):
    value: Any = None  # number for numeric metrics, a string for the key
    confidence: float | None = None
    quality_flag: str | None = None
    note: str | None = None


class MeasurementQualitySet(ResponseModel):
    loudness_lufs: QualifiedMeasurement | None = None
    true_peak_dbtp: QualifiedMeasurement | None = None
    crest_factor_db: QualifiedMeasurement | None = None
    bpm: QualifiedMeasurement | None = None
    key: QualifiedMeasurement | None = None


class MeasurementQualityResponse(ResponseModel):
    duration_sec: float | None = None
    measurements: MeasurementQualitySet | None = None
    n_reliable: int | None = None
    n_total: int | None = None


# --- /analyze/signal_integrity ------------------------------------------------


class TruePeakIntegrity(ResponseModel):
    true_peak_dbtp: float | None = None
    sample_peak_dbfs: float | None = None
    inter_sample_overshoot_db: float | None = None
    per_channel_dbtp: list[float] | None = None
    over_minus_1dbtp: bool | None = None
    verdict: str | None = None


class MonoBassIntegrity(ResponseModel):
    cutoff_hz: float | None = None
    correlation_below_cutoff: float | None = None
    energy_at_risk_db: float | None = None
    verdict: str | None = None


class NoiseFloorIntegrity(ResponseModel):
    noise_floor_dbfs: float | None = None
    hiss_headroom_db: float | None = None
    hum_detected: bool | None = None
    hum_hz: float | None = None
    verdict: str | None = None


class SignalIntegrityResponse(ResponseModel):
    true_peak: TruePeakIntegrity | None = None
    mono_bass: MonoBassIntegrity | None = None
    noise_floor: NoiseFloorIntegrity | None = None


# --- /analyze/pitch_accuracy --------------------------------------------------


class PitchAccuracyNote(ResponseModel):
    start_sec: float | None = None
    end_sec: float | None = None
    note_midi: int | None = None
    note_name: str | None = None
    mean_cents_deviation: float | None = None
    cents_stability: float | None = None


class PitchAccuracyResponse(ResponseModel):
    notes: list[PitchAccuracyNote] | None = None
    in_tune_pct: float | None = None
    mean_abs_cents: float | None = None
    worst_note: PitchAccuracyNote | None = None
    verdict: str | None = None
    reference_hz: float | None = None
    detected_reference_hz: float | None = None
    measurement_reference_hz: float | None = None
    reference_offset_cents: float | None = None
    reference_offset_warning: bool | None = None


# --- /analyze/quick_stats -----------------------------------------------------


class QuickStatsResponse(ResponseModel):
    format: str | None = None
    sample_rate_hz: int | None = None
    bit_depth: str | None = None
    channels: int | None = None
    duration_sec: float | None = None
    integrated_lufs: float | None = None
    loudness_range_lu: float | None = None
    true_peak_dbtp: float | None = None
    sample_peak_dbfs: float | None = None
    crest_factor_db: float | None = None
    dc_offset: float | None = None
    stereo_width_pct: float | None = None
    clipping_event_count: int | None = None
    clipping_positions: list[float] | None = None


# --- /analyze/stem_taxonomy ---------------------------------------------------


class StemTaxonomyBucket(ResponseModel):
    id: str | None = None
    label: str | None = None
    present: bool | None = None
    confidence: float | None = None
    rms_db: float | None = None
    energy_share_pct: float | None = None
    target_share_pct: float | None = None
    band_profile: list[float] | None = None


class StemTaxonomyCollision(ResponseModel):
    a: str | None = None
    b: str | None = None
    score: float | None = None


class StemTaxonomyResponse(ResponseModel):
    buckets: list[StemTaxonomyBucket] | None = None
    collisions: list[StemTaxonomyCollision] | None = None
    top_collisions: list[StemTaxonomyCollision] | None = None
    has_genre_target: bool | None = None
    taxonomy_note: str | None = None


# --- /humanize/* --------------------------------------------------------------


class CleanMetadataResponse(ResponseModel):
    output_path: str | None = None
    removed_tag_count: int | None = None
    removed_tags: list[str] | None = None
    kept_tags: list[str] | None = None
    # AI-provenance / disclosure tags: never removed, always reported.
    kept_provenance_tags: list[str] | None = None


class AnalogFeelResponse(ResponseModel):
    output_path: str | None = None
    amount: float | None = None
    applied: list[str] | None = None        # tape_drift / fader_ride / room_tone
    stem_aware: bool | None = None
    sample_rate_hz: int | None = None


class DeepHumanizeStemReport(ResponseModel):
    stem: str | None = None
    intensity: float | None = None
    channels: int | None = None


class DeepHumanizeResponse(ResponseModel):
    output_path: str | None = None
    sample_rate_hz: int | None = None
    stems_processed: int | None = None
    report: list[DeepHumanizeStemReport] | None = None


# --- /analyze/genre_context ---------------------------------------------------


class GenreCandidate(ResponseModel):
    genre: str | None = None
    subgenre: str | None = None
    rank_score: float | None = None
    group: list[str] | None = None  # GEN-1a: indistinguishable subgenres, likeliest first


class GenreContextEvidence(ResponseModel):
    bpm: float | None = None
    duration_sec: float | None = None
    rms_dbfs: float | None = None
    crest_factor_db: float | None = None
    spectral_centroid_hz: float | None = None
    spectral_rolloff_hz: float | None = None
    spectral_flatness: float | None = None
    spectral_contrast_db: float | None = None
    zero_crossing_rate: float | None = None
    onset_density_per_sec: float | None = None
    rhythm_peak_strength: float | None = None
    chroma_change_rate: float | None = None
    key_stability: float | None = None
    low_end_weight: float | None = None
    low_band_correlation: float | None = None
    mfcc_mean: float | None = None
    mfcc_variability: float | None = None
    stereo_correlation: float | None = None
    stereo_width: float | None = None


class GenreSectionCandidate(ResponseModel):
    start_sec: float | None = None
    end_sec: float | None = None
    top_candidate: str | None = None
    candidate_scores: list[GenreCandidate] | None = None
    evidence: GenreContextEvidence | None = None


class GenreProvenance(ResponseModel):
    target_source: str | None = None
    target_provenance: str | None = None
    measurement_protocol: str | None = None
    benchmark_distribution: str | None = None
    sample_size: int | None = None
    is_reference_data: bool | None = None
    note: str | None = None


class GenreSourceQualityContext(ResponseModel):
    triage_verdict: str | None = None
    integrity_warnings: list[Any] | None = None


class GenreUserConfirmation(ResponseModel):
    stated_genre_id: str | None = None
    use_genre_targets: bool | None = None
    detected_candidate_confirmed: bool | None = None
    selection_mode: str | None = None
    metadata: dict[str, Any] | None = None   # user-supplied free-form metadata


class GenreQuickSection(ResponseModel):
    label: str | None = None
    start_sec: float | None = None


class GenreQuickAnalysis(ResponseModel):
    bpm: float | None = None
    bpm_confidence: float | None = None
    key: str | None = None
    mode: str | None = None
    key_confidence: float | None = None
    energy: float | None = None
    mood: str | None = None
    detected_genre: str | None = None
    detected_subgenre: str | None = None
    beat_times: list[float] | None = None
    downbeat_times: list[float] | None = None
    sections: list[GenreQuickSection] | None = None
    pitch_notes: list[Any] | None = None
    analysis_duration_ms: float | None = None


class GenreFamilyCandidate(ResponseModel):
    family: str | None = None
    probability: float | None = None


class GenreClassifierAccuracy(ResponseModel):
    """Cross-validated accuracy of the genre probe on its training corpus."""
    family_top1: float | None = None
    genre_top1: float | None = None
    genre_top5: float | None = None
    majority_family_baseline: float | None = None
    group_top1: float | None = None  # GEN-1a: the true genre's group ranked first
    group_top5: float | None = None
    tracks: int | None = None


class GenreContextResponse(ResponseModel):
    file_path: str | None = None
    cached: bool | None = None
    selected_genre_id: str | None = None
    selected_source: str | None = None
    candidates: list[GenreCandidate] | None = None
    section_candidates: list[GenreSectionCandidate] | None = None
    evidence: GenreContextEvidence | None = None
    classifier_status: str | None = None
    is_hybrid_or_conflicted: bool | None = None
    genre_relative_action_allowed: bool | None = None
    abstained_reason: str | None = None
    reference_available: bool | None = None
    reference_path: str | None = None
    provenance: GenreProvenance | None = None
    source_quality_context: GenreSourceQualityContext | None = None
    user_confirmation: GenreUserConfirmation | None = None
    quick_analysis: GenreQuickAnalysis | None = None
    # NEW-9: set only when the CLAP genre probe produced the candidates.
    family: str | None = None
    family_probability: float | None = None
    families: list[GenreFamilyCandidate] | None = None
    classifier_accuracy: GenreClassifierAccuracy | None = None
    personal_examples: int | None = None  # GEN-1e: the user's examples that moved the suggestion


# --- sound_recreation, midi_variation, vocal_harmony ----


class SoundRecreationFeatures(ResponseModel):
    centroid_hz: float | None = None
    rolloff_hz: float | None = None
    bandwidth_hz: float | None = None
    flatness: float | None = None
    zcr: float | None = None
    harmonic_ratio: float | None = None
    sub_fraction: float | None = None
    attack_sec: float | None = None


class SoundRecreationMidiSummary(ResponseModel):
    kind: str | None = None
    extraction_backend: str | None = None
    low_quality_extraction: bool | None = None
    has_glide: bool | None = None
    n_notes: int | None = None
    pitch_min_midi: int | None = None
    pitch_max_midi: int | None = None
    n_hits: int | None = None


class SoundRecreationRecommendation(ResponseModel):
    plugin: str | None = None
    patch: str | None = None
    chain: str | None = None
    why: str | None = None


class SoundRecreationResponse(ResponseModel):
    category: str | None = None
    features: SoundRecreationFeatures | None = None
    midi_summary: SoundRecreationMidiSummary | None = None
    low_quality_extraction: bool | None = None
    recommendations: list[SoundRecreationRecommendation] | None = None


class MidiVariationEvent(ResponseModel):
    start_sec: float | None = None
    end_sec: float | None = None
    time_sec: float | None = None
    midi: int | None = None
    note: str | None = None
    kind: str | None = None
    velocity: int | None = None


class MidiVariationResponse(ResponseModel):
    output_path: str | None = None
    kind: str | None = None
    extraction_backend: str | None = None
    low_quality_extraction: bool | None = None
    n_events: int | None = None
    events: list[MidiVariationEvent] | None = None
    bpm: float | None = None
    bpm_source: str | None = None
    key: str | None = None
    time_signature: str | None = None
    # Seconds added to every note in the .mid so bar 1 falls on the first
    # detected downbeat (0.0 when none was detected).
    midi_time_offset_sec: float | None = None


class VocalHarmonyEvent(ResponseModel):
    start_sec: float | None = None
    end_sec: float | None = None
    source_midi: int | None = None
    source_note: str | None = None
    harmony_midi: int | None = None
    harmony_note: str | None = None
    velocity: int | None = None


class VocalHarmonyResponse(ResponseModel):
    output_path: str | None = None
    key: str | None = None
    mode: str | None = None
    key_source: str | None = None
    key_confidence: float | None = None
    interval: str | None = None
    direction: str | None = None
    bpm: float | None = None
    bpm_source: str | None = None
    time_signature: str | None = None
    midi_time_offset_sec: float | None = None
    extraction_backend: str | None = None
    low_quality_extraction: bool | None = None
    n_events: int | None = None
    events: list[VocalHarmonyEvent] | None = None


# --- /analyze/fingerprint, fingerprint_compare --------------------------------


class AcoustIdMatch(ResponseModel):
    title: str | None = None
    artist: str | None = None
    mbid: str | None = None
    score: float | None = None


class FingerprintOnlineLookup(ResponseModel):
    status: str | None = None
    detail: str | None = None
    results: list[AcoustIdMatch] | None = None


class FingerprintResponse(ResponseModel):
    method: str | None = None
    chromaprint_available: bool | None = None
    fingerprint: str | None = None
    n_frames: int | None = None
    duration_sec: float | None = None
    note: str | None = None
    online_lookup: FingerprintOnlineLookup | None = None


class FingerprintCompareResponse(ResponseModel):
    method: str | None = None
    similarity: float | None = None
    verdict: str | None = None
    n_frames_a: int | None = None
    n_frames_b: int | None = None


# --- /analyze/models/watermark/* ----------------------------------------------


class WatermarkSentinel(ResponseModel):
    cached: bool | None = None
    at: float | None = None
    error: str | None = None


class WatermarkStatusResponse(ResponseModel):
    status: str | None = None
    current_model: str | None = None
    done: int | None = None
    total: int | None = None
    error: str | None = None
    packages_available: dict[str, bool] | None = None   # model id -> importable
    cached: dict[str, bool] | None = None               # model id -> weights cached
    sentinel: dict[str, WatermarkSentinel] | None = None


class WatermarkPrecacheResponse(WatermarkStatusResponse):
    started: bool | None = None


# --- /analyze/models/manifest, registry, check_updates ------------------------


class ModelRegistryInfo(ResponseModel):
    description: str | None = None
    produced_by: str | None = None
    license: str | None = None


class ModelManifestEntry(ResponseModel):
    name: str | None = None
    filename: str | None = None
    installed: bool | None = None
    available: bool | None = None
    unavailable_reason: str | None = None
    remediation_hint: str | None = None
    intervention_validated: bool | None = None
    # provenance / version / source_url / val_* come from the model's sidecar
    # JSON when it has them (else the bundled registry), so their types are
    # whatever the sidecar wrote.
    provenance: Any = None
    version: Any = None
    source_url: Any = None
    size_bytes: int | None = None
    sha256: str | None = None
    downloaded_at: str | None = None
    metadata: dict[str, Any] | None = None   # the model's sidecar JSON, verbatim
    val_accuracy: Any = None
    val_ece: Any = None
    expected_calibration_error: Any = None
    registry: ModelRegistryInfo | None = None


class ModelManifestResponse(ResponseModel):
    models_dir: str | None = None
    count: int | None = None
    available_count: int | None = None
    unavailable_count: int | None = None
    any_unvalidated: bool | None = None
    models: list[ModelManifestEntry] | None = None


class ModelRegistryEntry(ResponseModel):
    provenance: str | None = None
    source_url: str | None = None
    description: str | None = None
    produced_by: str | None = None
    license: str | None = None


class ModelRegistryResponse(ResponseModel):
    # "schema" would shadow a BaseModel attribute, hence the alias.
    schema_version: int | None = Field(default=None, alias="schema")
    note: str | None = None
    models: dict[str, ModelRegistryEntry] | None = None   # model id -> entry


class ModelUpdateEntry(ResponseModel):
    name: str | None = None
    source_url: Any = None
    local_version: Any = None
    remote_version: Any = None
    status: str | None = None
    error: str | None = None


class ModelUpdatesResponse(ResponseModel):
    online: bool | None = None
    any_update_available: bool | None = None
    models: list[ModelUpdateEntry] | None = None


# --- /analyze/codec/* ---------------------------------------------------------


class CodecPlatform(ResponseModel):
    id: str | None = None
    name: str | None = None
    codec: str | None = None
    bitrate_k: int | None = None
    ext: str | None = None


class CodecPlatformsResponse(ResponseModel):
    platforms: list[CodecPlatform] | None = None


class CodecAuditResult(ResponseModel):
    codec_id: str | None = None
    name: str | None = None
    status: str | None = None
    error: str | None = None
    decoded_path: str | None = None
    hf_loss_db: float | None = None
    air_loss_db: float | None = None
    stereo_width_delta: float | None = None
    true_peak_delta_dbtp: float | None = None
    pre_echo_score: float | None = None


class CodecAuditResponse(ResponseModel):
    encoder_available: bool | None = None
    codecs: list[str] | None = None
    reason: str | None = None
    results: list[CodecAuditResult] | None = None


# --- /analyze/dither/preview --------------------------------------------------


class DitherMode(ResponseModel):
    mode: str | None = None
    label: str | None = None
    noise_floor_dbfs: float | None = None
    distortion_correlation: float | None = None


class DitherPreviewResponse(ResponseModel):
    source_bits: int | None = None
    target_bits: int | None = None
    sample_rate: int | None = None
    dither_needed: bool | None = None
    modes: list[DitherMode] | None = None
    noise_floor_delta_db: float | None = None
    recommendation: str | None = None
    audition_path: str | None = None


# --- /analyze/groove_timing, stem_phase, reverb_tail, harmony_timeline --------


class GrooveEvent(ResponseModel):
    time_sec: float | None = None
    offset_ms: float | None = None


class GrooveTimingResponse(ResponseModel):
    bpm: float | None = None
    grid_offset_ms: float | None = None
    timing_std_ms: float | None = None
    ahead_fraction: float | None = None
    behind_fraction: float | None = None
    swing_ratio: float | None = None
    pocket: str | None = None
    grid_subdivision: str | None = None
    events: list[GrooveEvent] | None = None


class StemPhasePair(ResponseModel):
    left: str | None = None
    right: str | None = None
    correlation: float | None = None
    polarity: str | None = None


class StemPhaseResponse(ResponseModel):
    pairs: list[StemPhasePair] | None = None
    stem_count: int | None = None


class ReverbTailResponse(ResponseModel):
    rt60_sec: float | None = None
    fit_r2: float | None = None
    tail_energy_ratio: float | None = None
    source_reverb_tail: str | None = None
    decay_segments: int | None = None
    rt60_note: str | None = None


class HarmonyKeyPoint(ResponseModel):
    time_sec: float | None = None
    key: str | None = None
    mode: str | None = None


class HarmonyChord(ResponseModel):
    time_sec: float | None = None
    chord: str | None = None
    confidence: float | None = None


class HarmonyTimelineResponse(ResponseModel):
    initial_key: HarmonyKeyPoint | None = None
    key_changes: list[HarmonyKeyPoint] | None = None
    progression: list[HarmonyChord] | None = None
