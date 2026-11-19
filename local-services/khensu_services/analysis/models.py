from __future__ import annotations

from typing import Literal

from pydantic import Field

from khensu_services.common.api_models import RequestModel, ResponseModel


class AnalyzeRequest(RequestModel):
    file_path: str
    mode: Literal["quick", "advanced"] = "quick"
    # Read by advanced /analyze/start jobs only. "fix_check" is the desktop's
    # check of a fixed version (FEAT-1): it skips the extras no finding is
    # built from (emotional arc, translation prediction, stereo-field
    # timeline, benchmark) - about half the pass on a long track.
    purpose: Literal["full", "fix_check"] = "full"


class SectionMarker(ResponseModel):
    start_sec: float
    label: str
    # NEW-1: the section's role (intro | verse | pre-chorus | chorus | bridge |
    # outro | section), its repetition group (A, B, ...) and the novelty peak
    # at its start (0-1). Absent on the fallback grid.
    role: str | None = None
    group: str | None = None
    boundary_strength: float | None = None
    # Per-section integrated LUFS (advanced mode). -70.0 = silent; None = not
    # measured (section shorter than 4 s, or the measurement failed) - a loud
    # short break must not read as silence (DSP-12).
    lufs_integrated: float | None = None


class PitchNote(ResponseModel):
    start_sec: float
    end_sec: float
    midi_pitch: int
    velocity: float
    # True for chroma-fallback notes: octave-less, written at 60 + pitch
    # class - consumers must not read their register (no chord bass, no
    # melodic leaps). None / absent for real transcriptions.
    pitch_class_only: bool | None = None


class HookInstance(ResponseModel):
    start_sec: float
    end_sec: float
    hook_type: Literal["topline", "backing", "structural", "compound"]
    strength: float


class AnalyzeResponse(ResponseModel):
    mode: Literal["quick", "advanced"]
    bpm: float
    bpm_confidence: float
    beat_times: list[float]
    downbeat_times: list[float] | None
    key: str
    key_confidence: float
    sections: list[SectionMarker]
    pitch_notes: list[PitchNote] | None
    analysis_duration_ms: float
    detected_genre: str | None = None
    detected_subgenre: str | None = None
    # Waveform symmetry (advanced mode). advanced_analyzer always computed
    # these, but they weren't declared here, so the response model dropped
    # them and the C++ fields stayed at their defaults forever.
    skewness: float = 0.0
    peak_asymmetry_ratio: float = 1.0
    is_asymmetric: bool = False
    # SP-06 — present in advanced mode only, None in quick mode
    hook_instances:       list[HookInstance] | None = None
    hook_strength:        float | None = None
    hook_density:         float | None = None
    hook_count:           int   | None = None
    memorability_score:   float | None = None
    earworm_likelihood:   float | None = None
    melody_salience:      float | None = None
    arc_quality:          float | None = None
    heaviness_distortion: float | None = None
    heaviness_weight:     float | None = None
    heaviness_clarity:    float | None = None
    heaviness_extremity:  float | None = None
    # Meter (advanced mode; None in quick mode, which assumes 4/4).
    # time_signature is "4/4", "3/4" or "6/8"; a 4/4 reading with low
    # meter_confidence (<= 0.2) is the fallback, not a detection.
    # downbeat_times follow the detected beats-per-bar.
    time_signature:       str   | None = None
    meter_confidence:     float | None = None
    # Key relationships for `key` (advanced mode; None when unparseable).
    camelot:                str | None = None   # e.g. "8B" (C major), "8A" (A minor)
    circle_of_fifths_index: int | None = None   # 0..11 clockwise from C major / A minor
    relative_key:           str | None = None   # e.g. "A minor"
    parallel_key:           str | None = None   # e.g. "C minor"
    # Mood/energy (advanced mode). The analyzers always returned these and
    # the C++ IpcTypes reads them, but undeclared they were dropped here.
    energy:                 float | None = None
    mood:                   str | None = None
    # Dotted-quarter ("felt") tempo, only when the meter reads 6/8 at the
    # eighth-note pulse (6 beats per bar); None otherwise.
    felt_bpm:               float | None = None
    # Quarter-note (DAW-session / MIDI) tempo (advanced mode; THY-09): equals
    # bpm in simple meters; bpm / 2 for 6/8 tracked at the eighth pulse, bpm x
    # 1.5 for 6/8 tracked at the dotted quarter. Genre matching, mood and
    # memorability use this value (genre_targets.json quotes quarter-note
    # session tempos).
    quarter_note_bpm:       float | None = None


class MonoCompatResult(ResponseModel):
    per_band_correlation: dict[str, float]
    low_end_stereo_pct: float


class ResonanceHotspot(ResponseModel):
    hz: float
    db: float = 0.0          # absolute magnitude in dB at this frequency
    prominence_db: float     # prominence above surrounding spectrum


class VocalClarityResult(ResponseModel):
    presence_score: float   # 0–1: energy in 1–5 kHz relative to full mid band
    clarity_score: float    # 0–1: presence-to-mud ratio
    presence_ratio: float   # raw 1–5 kHz / 200 Hz–8 kHz energy
    mud_ratio: float        # raw 300 Hz–1 kHz / 200 Hz–8 kHz energy


class MidSideImbalanceResult(ResponseModel):
    side_bright_ratio: float        # side/mid energy ratio above 4 kHz
    side_heavy_ratio: float         # side/mid energy ratio below 100 Hz
    brightness_imbalance_db: float  # dB(side) − dB(mid) in high band
    heaviness_imbalance_db: float   # dB(side) − dB(mid) in low band
    imbalance_severity: float       # 0–1 composite severity
    warning: str | None = None


class EmotionalArcPoint(ResponseModel):
    window_index: int
    progress_fraction: float
    energy_0_100: float
    mood: str
    mood_tags: list[str] | None = None


class TrackMood(ResponseModel):
    """NEW-9: the CLAP mood probe's whole-track tags (experimental)."""
    tags: list[str]
    probabilities: dict[str, float]
    source: str = "clap_probe"


class EmotionalArcData(ResponseModel):
    points: list[EmotionalArcPoint]
    flat_sections: list[tuple[float, float]] = []
    max_energy_delta: float = 0.0
    # NEW-9: whole-track mood from the CLAP probe, when CLAP and the probe are
    # present (held-out balanced accuracy 0.70 vs the feature tagger's 0.62).
    track_mood: TrackMood | None = None


class GenreBlendEntry(ResponseModel):
    genre: str
    subgenre: str
    rank_score: float  # heuristic relative rank; explicitly not calibrated confidence
    # GEN-1a: subgenres the probe cannot tell apart, likeliest first; rank_score is their total.
    group: list[str] | None = None


class TranslationCollision(ResponseModel):
    band_a_hz: tuple[float, float]
    band_b_hz: tuple[float, float]
    collision_score: float
    increase: float


class TranslationProfileScore(ResponseModel):
    degradation_score: float
    new_masking_collisions: list[TranslationCollision] = []


class TranslationPredictionData(ResponseModel):
    per_profile: dict[str, TranslationProfileScore]


class BenchmarkData(ResponseModel):
    genre_id: str
    percentile_by_metric: dict[str, float]
    competitive_summary: str
    sample_size: int
    is_bootstrap_data: bool = False


class StereoFieldPoint(ResponseModel):
    window_index: int
    progress_fraction: float
    correlation: float
    width: float
    sub_bass_mono_pct: float


class StereoFieldTimeline(ResponseModel):
    points: list[StereoFieldPoint] = []


class ExtendedAnalysisResponse(AnalyzeResponse):
    """Extends the standard analysis response with SP-16C fields."""
    harshness_score: float = 0.0
    harshness_peak_hz: float | None = None
    harshness_peak_db: float | None = None
    sibilance_score: float | None = None
    sibilance_peak_hz: float | None = None
    vocal_clarity: VocalClarityResult | None = None
    mono_compat: MonoCompatResult | None = None
    mid_side_imbalance: MidSideImbalanceResult | None = None
    translation_risk: dict[str, float] = {}
    resonance_hotspots: list[ResonanceHotspot] = []
    # 2026-07-09 AI-analysis-deepening additions
    emotional_arc: EmotionalArcData | None = None
    genre_blend: list[GenreBlendEntry] = []
    # Genre provenance + confirmation (2026-07-15 safety audit P1): detected
    # genre is a candidate, targets are hand-tuned defaults. Consumers must not
    # apply genre-relative settings automatically until confirmed.
    genre_provenance: dict | None = None
    genre_confirmation: dict | None = None
    genre_abstained_reason: str | None = None
    genre_relative_action_allowed: bool = False
    genre_is_hybrid: bool = False
    translation_prediction: TranslationPredictionData | None = None
    benchmark: BenchmarkData | None = None
    stereo_field_timeline: StereoFieldTimeline | None = None


class SectionAnalysisResult(ResponseModel):
    label: str
    role: str | None = None   # NEW-1, see SectionMarker
    group: str | None = None
    start_sec: float
    duration_sec: float
    lufs: float | None = None
    crest_db: float | None = None
    lra: float | None = None
    stereo_correlation: float | None = None
    balance_low: float | None = None
    dominant_freq_hz: float | None = None
    harshness_score: float | None = None    # 0–1: 2.5–5 kHz vs. 1–2.5 kHz ratio
    noise_floor_db: float | None = None     # dBFS: 10th-pct RMS of 50 ms windows
    peak_dbfs: float | None = None          # dBFS sample peak (not true peak)
    sibilance_score: float | None = None    # 0–1: 5–10 kHz vs. 1–10 kHz energy ratio
    boundary_confidence: float | None = None  # 0–1: onset flux strength at this section boundary


class LoudnessArcResult(ResponseModel):
    chorus_verse_delta_lu: float | None = None  # loudest − quietest section LUFS (proxy for energy management)
    max_section_lufs: float | None = None
    min_section_lufs: float | None = None
    section_count: int = 0


class SectionsAnalysisRequest(RequestModel):
    file_path: str
    mode: Literal["quick", "advanced"] = "quick"


class SectionsAnalysisResponse(AnalyzeResponse):
    section_analysis: list[SectionAnalysisResult] = []
    loudness_arc: LoudnessArcResult | None = None


class FileIntegrityRequest(RequestModel):
    file_path: str


class FileIntegrityResult(ResponseModel):
    sha256: str
    nominal_sample_rate: int
    nominal_bit_depth: str
    nominal_channels: int = 2
    duration_sec: float
    suspect_upsample: bool
    upsample_hf_ratio: float
    suspect_16bit_padded: bool
    lsb_residual_rms: float
    suspect_fake_stereo: bool = False
    warnings: list[str] = []


class ReferenceAnalysisRequest(RequestModel):
    target_path: str
    reference_path: str


class ReferenceTimeWindow(ResponseModel):
    window_index: int
    target_start_sec: float
    target_end_sec: float
    reference_start_sec: float
    reference_end_sec: float
    target_centroid_hz: float
    reference_centroid_hz: float
    delta_centroid_hz: float
    target_crest_db: float
    reference_crest_db: float
    delta_crest_db: float
    delta_octave_db: list[float]  # target - reference, same 10 bands as octave_center_hz


class ReferenceAnalysisResponse(ResponseModel):
    target_lufs: float
    reference_lufs: float
    delta_lufs: float        # target - reference (+ = target louder)
    profile_recommendations: list[str] = []  # human-readable dynamic-profile mismatch notes

    target_lra: float
    reference_lra: float
    delta_lra: float

    target_crest_db: float
    reference_crest_db: float
    delta_crest_db: float

    target_centroid_hz: float
    reference_centroid_hz: float
    delta_centroid_hz: float

    target_stereo_correlation: float
    reference_stereo_correlation: float
    delta_stereo_correlation: float

    target_balance_low: float
    reference_balance_low: float
    delta_balance_low: float

    # Per-octave spectral comparison (10 standard 1/1-octave bands, IEC 1260).
    # Values are the fraction of total energy in that band, expressed in dB.
    # Delta is target - reference: positive means target is brighter/heavier in that band.
    octave_center_hz: list[float] = []
    target_octave_rms_db: list[float] = []
    reference_octave_rms_db: list[float] = []
    delta_octave_db: list[float] = []

    # Suggested corrective EQ to move the target toward the reference tonal
    # balance: gain_db = -(target - reference) per octave band, clamped to a
    # musical range so a single wild band can't demand +18 dB. Ready to render
    # as a curve or feed to apply_graph.
    match_eq_center_hz: list[float] = []
    match_eq_gain_db: list[float] = []

    # Dynamic (time-varying) comparison: the whole-file numbers above average
    # away section-to-section drift (e.g. a chorus that's brighter than the
    # reference's chorus even though the track-wide centroid matches). Each
    # window pairs target window i against reference window i by song-
    # progress fraction (not wall-clock time, since the two tracks can have
    # different lengths). Empty when either track is too short to window
    # meaningfully.
    time_windows: list[ReferenceTimeWindow] = []


class DrumEventResult(ResponseModel):
    onset_sec: float
    attack_ms: float
    body_db:   float
    fund_hz:   float


class DrumPartResult(ResponseModel):
    count:             int
    # Hit RATE in hits per minute (60 / mean inter-onset interval), NOT the
    # track tempo: kicks on beats 1 and 3 at 120 BPM read 60 (THY-11). The
    # name is kept for wire compatibility; display it as "kicks/min".
    avg_bpm:           float
    timing_regularity: float   # 0–1; 1 = perfectly regular
    avg_body_db:       float
    avg_fund_hz:       float
    events:            list[DrumEventResult] = []


class DrumAnalysisRequest(RequestModel):
    file_path: str


class DrumAnalysisResponse(ResponseModel):
    kick:                DrumPartResult
    snare:               DrumPartResult
    transient_sharpness:       float          # 0–1: fraction of events with attack < 10 ms
    kick_bass_proximity_hz:    float = 0.0   # |kick_fund_hz − bass_fund_hz|
    kick_bass_frequency_clash: bool  = False  # True when proximity < 10 Hz
    bass_fundamental_hz:       float = 0.0   # dominant sustained sub-bass frequency


class SeparateRequest(RequestModel):
    file_path: str
    # Empty/omitted → an AMAS_<M>_<D>_<YYYY>_<SongTitle> folder is created
    # next to the source file (spec §1.3, util/naming.py).
    output_dir: str = ""
    # Backend (CE-AI-4): "roformer" tries BS-/Mel-RoFormer, then Demucs, then
    # HPSS; "auto" is the same chain; "demucs" tries Demucs, then HPSS.
    method: Literal["hpss", "demucs", "roformer", "auto"] = "hpss"
    # Which RoFormer checkpoint ("" = bs_roformer); ignored by other methods.
    roformer_model: Literal["", "bs_roformer", "mel_roformer"] = ""


class SeparateResponse(ResponseModel):
    vocals_path: str
    drums_path: str
    bass_path: str
    other_path: str
    method_used: str = "hpss"
    separation_quality_score: float = 0.0  # 0–1; 1 = well-separated stems, 0 = high leakage
    model: str | None = None       # checkpoint for a model separation (CE-AI-4)
    device: str | None = None      # torch device it ran on (CE-PLT-1)


# --- Masking & frequency collision detector ---

class MaskingRequest(RequestModel):
    vocals_path: str = ""
    drums_path: str = ""
    bass_path: str = ""
    other_path: str = ""


class MaskingWindow(ResponseModel):
    start_sec: float
    end_sec: float
    collision_score: float


class MaskingPair(ResponseModel):
    stem_a: str
    stem_b: str
    collision_score: float           # 0–1; 1 = fully overlapping band-energy distribution
    band_scores: list[float]         # per-band min(a,b) energy fraction, same order as bands_hz
    collisions_over_time: list[MaskingWindow] = []  # same score, windowed to localize hotspots


class SubBassPhaseRequest(RequestModel):
    kick_path: str   # drums stem is fine — the analyzer low-passes to the kick
    bass_path: str
    cutoff_hz: float = 120.0


class PhaseCorrelationPoint(ResponseModel):
    time_sec: float
    correlation: float          # -1..1 over the low band in this window


class SubBassPhaseResponse(ResponseModel):
    cutoff_hz: float
    correlation: float          # -1..1 whole-track low-band correlation
    phase_degrees: float        # dominant kick vs bass phase; 0 = in phase
    verdict: str
    correlation_over_time: list[PhaseCorrelationPoint] = []
    goniometer: list[list[float]] = []  # decimated [kick, bass] points, -1..1


class MaskingHeatmap(ResponseModel):
    stem_a: str
    stem_b: str
    block_ms: float                  # time resolution per column
    n_frames: int
    bands_hz: list[float]            # lower edge of each band, Hz (rows)
    grid: list[list[float]]          # [frame][band] = 0..1 collision intensity


class MaskingResponse(ResponseModel):
    pairs: list[MaskingPair]
    bands_hz: list[float]            # lower edge of each band, Hz
    # Sub-second time x frequency masking heatmap for the worst-colliding pair
    # (Gemini audit: transient-scale dynamic masking).
    heatmap: MaskingHeatmap | None = None


# --- Spectro-temporal heatmap (Timeline tab) ---

class SpectrotemporalRequest(RequestModel):
    file_path: str
    n_bands: int = Field(default=24, ge=4, le=128)
    target_frames: int = Field(default=200, ge=8, le=2000)


class SpectrotemporalResponse(ResponseModel):
    duration_sec: float
    n_bands: int
    band_edges_hz: list[float]
    n_frames: int
    frames: list[list[float]]        # [frame][band] = 0..1 normalized magnitude


class LoudnessCurveRequest(RequestModel):
    file_path: str


class MetricTimelineRequest(RequestModel):
    file_path: str
    block_sec: float = Field(3.0, gt=0.0, le=30.0)
    hop_sec: float = Field(1.0, gt=0.0, le=30.0)
    # CE-PLT-2: a post-render re-measure passes the source here; only the
    # windows that touch a changed region of file_path are recomputed.
    baseline_path: str = ""


class MixQualityRequest(RequestModel):
    file_path: str


class ReferenceMatchRequest(RequestModel):
    file_path: str
    genre_id: str = ""
    k: int = Field(5, ge=1, le=25)
    include_sections: bool = False


class EvidenceGraphRequest(RequestModel):
    file_path: str
    stems: dict[str, str] | None = None


class MeasurementQualityRequest(RequestModel):
    file_path: str


class StreamingPlatformResult(ResponseModel):
    id: str
    name: str
    target_lufs: float
    applied_gain_db: float          # what the platform will do to the master
    resulting_lufs: float
    resulting_true_peak_dbtp: float
    headroom_db: float              # below the platform's true-peak ceiling
    turned_down: bool
    turned_up: bool
    limiter_engaged: bool


class ReferenceSpectralDeltaRequest(RequestModel):
    target_path: str
    reference_path: str
    n_bands: int = Field(default=28, ge=4, le=128)
    target_frames: int = Field(default=300, ge=8, le=2000)


class ReferenceSpectralDeltaResponse(ResponseModel):
    duration_sec: float
    n_bands: int
    band_edges_hz: list[float]
    n_frames: int
    grid: list[list[float]]          # [frame][band] dB delta, +hot / -shy vs reference


class StemEnergyTimelineRequest(RequestModel):
    vocals_path: str = ""
    drums_path: str = ""
    bass_path: str = ""
    other_path: str = ""


class StemEnergyFrame(ResponseModel):
    time_sec: float
    fractions: dict[str, float]     # stem -> 0..1 share (sums to 1)


class StemEnergyTimelineResponse(ResponseModel):
    stems: list[str]
    hop_sec: float
    n_frames: int
    frames: list[StemEnergyFrame]


class PhaseHeatmapRequest(RequestModel):
    file_path: str
    n_bands: int = Field(default=28, ge=4, le=128)
    target_frames: int = Field(default=300, ge=8, le=2000)


class PhaseHeatmapResponse(ResponseModel):
    duration_sec: float
    n_bands: int
    band_edges_hz: list[float]
    n_frames: int
    # [frame][band] correlation -1..1; null = no energy in that cell (DSP-16)
    grid: list[list[float | None]]
    is_stereo: bool


class MidSideSpectrogramRequest(RequestModel):
    file_path: str
    n_bands: int = Field(default=32, ge=4, le=128)
    target_frames: int = Field(default=300, ge=8, le=2000)


class MidSideSpectrogramResponse(ResponseModel):
    duration_sec: float
    n_bands: int
    band_edges_hz: list[float]
    n_frames: int
    mid_frames: list[list[float]]     # [frame][band] 0..1
    side_frames: list[list[float]]    # [frame][band] 0..1


class VocalHarmonyRequest(RequestModel):
    """Export a diatonic backing-harmony MIDI line from an extracted vocal.

    ``key`` / ``bpm`` left out are detected from the audio (the response's
    ``key_source`` / ``bpm_source`` say which) - never silently C major /
    120 BPM; an undetectable one is a 422 asking for it. ``key`` is a note
    name ("F#", "Bb") or a key ("F# minor", "F#m"); ``mode`` needs a key."""
    file_path: str
    key: str | None = None     # C, C#, Db ...; normalized by vocal_harmony.py
    mode: Literal["major", "minor"] | None = None
    interval: Literal["third", "fifth"] = "third"
    direction: Literal["above", "below"] = "above"
    bpm: float | None = Field(default=None, ge=20.0, le=300.0)
    output_path: str = ""


class SoundRecreationRequest(RequestModel):
    file_path: str
    stem_type: str = "auto"
    # Concert A for the pitched MIDI extraction (AUDIT-016); None = 440.
    reference_hz: float | None = Field(default=None, ge=400.0, le=480.0)


class FingerprintRequest(RequestModel):
    file_path: str
    online: bool = True


class FingerprintCompareRequest(RequestModel):
    file_path_a: str
    file_path_b: str


class MonoCompatRequest(RequestModel):
    file_path: str


class MonoCompatResponse(ResponseModel):
    is_stereo: bool
    bands: list[dict]         # band_hz, correlation, fold_loss_db
    systems: list[dict]       # id, name, score (0-100), worst_band_hz, worst_band_loss_db


class TuningReferenceRequest(RequestModel):
    file_path: str


class TuningDriftPoint(ResponseModel):
    time_sec: float
    cents: float


class TuningReferenceResponse(ResponseModel):
    reference_hz: float
    cents_from_440: float
    verdict: str
    drift_range_cents: float
    drifting: bool
    drift: list[TuningDriftPoint] = []
    # THY-12: the estimate is modulo one semitone; these references (a
    # semitone below / above) read identically. caveat says so in words.
    semitone_alternatives_hz: list[float] = []
    caveat: str | None = None


class SourceForensicsRequest(RequestModel):
    file_path: str


class SourceForensicsResponse(ResponseModel):
    sample_rate: int
    declared_bits: int
    subtype: str
    brickwall: dict           # bandwidth_hz, brickwall_detected, likely_source
    hi_res: dict              # applicable, ultrasonic_ratio_db, upsampled_suspected
    bit_depth: dict           # declared_bits, noise_floor_dbfs, verdict, evidence


class StreamingNormalizationRequest(RequestModel):
    file_path: str


class StreamingNormalizationResponse(ResponseModel):
    integrated_lufs: float          # -70.0 when not measurable (see measurable)
    true_peak_dbtp: float
    # DSP-09: False for a silent / < 400 ms file - every platform then
    # applies 0 dB (it used to show a +56 dB "turn-up").
    measurable: bool = True
    note: str | None = None
    platforms: list[StreamingPlatformResult]


class TruePeakEvent(ResponseModel):
    time_sec: float           # when the overshoot peaks
    dbtp: float               # inter-sample peak magnitude, dBTP


class LoudnessCurveResponse(ResponseModel):
    duration_sec: float
    hop_sec: float            # spacing between samples, in seconds
    block_sec: float          # K-weighted analysis window per sample
    # short-term LUFS, one value per hop; null = digitally silent window (DSP-13)
    lufs_curve: list[float | None]
    # rolling peak/RMS crest factor (dB), same hop; null below the -90 dBFS floor
    crest_curve: list[float | None] = []
    # True-peak overshoot distribution (Gemini audit): inter-sample peaks over
    # the ceiling, with when + how hot, for the loudness-timeline overlay.
    true_peak_threshold_dbtp: float = -1.0
    true_peak_over_count: int = 0
    true_peak_events: list[TruePeakEvent] = []


# --- Chord analysis (SP-23) ---

class ChordsRequest(RequestModel):
    file_path: str


class ChordEvent(ResponseModel):
    time_sec: float
    chord: str          # Root + quality spelled for the key, e.g. "C", "Am", "Bb7",
                        # "F#m7", "Cmaj7", "Bdim", "Dsus4"; "N" = no chord
    confidence: float   # Cosine similarity vs. chord template [0.0, 1.0]; 0 for "N"
    # Roman numeral relative to the response key, e.g. "I", "vi", "V7",
    # "bVII" (borrowed), "#ivo"; None for "N" or when the key is unknown.
    roman: str | None = None
    # True when all chord tones are in the key (minor: natural or harmonic);
    # None for "N" / unknown key.
    diatonic: bool | None = None
    # Harmonic function (THY-05): "diatonic", "secondary" (applied dominant /
    # leading-tone chord, e.g. D7 = V7/V in C), "borrowed" (modal mixture,
    # e.g. Bb = bVII in C) or "chromatic"; None for "N" / unknown key.
    function: str | None = None


class ChordsResponse(ResponseModel):
    chords: list[ChordEvent]
    key: str            # Overall key root, e.g. "A", "Bb"; "unknown" when silent
    mode: str           # "major" or "minor"; "" when the key is unknown
    key_confidence: float
    # Key relationships (None when the key is unknown).
    camelot: str | None = None                 # Camelot wheel code, e.g. "8B"
    circle_of_fifths_index: int | None = None  # 0..11 clockwise from C major / A minor
    relative_key: str | None = None            # e.g. "A minor" for C major
    parallel_key: str | None = None            # e.g. "C minor" for C major


# --- Vocal-specific time-resolved analysis (/analyze/vocal) ---
class VocalRequest(RequestModel):
    file_path: str


class SibilanceSample(ResponseModel):
    time_sec: float
    sibilance: float      # 0..1


class BreathMarker(ResponseModel):
    time_sec: float
    duration_sec: float


class PlosiveMarker(ResponseModel):
    """A candidate low-frequency vocal plosive, not a speech classification."""
    time_sec: float
    duration_sec: float
    severity: float              # 0..1
    low_band_ratio: float        # 0..1, 40-220 Hz share of low/mid energy


class SiiSample(ResponseModel):
    time_sec: float
    sii: float


class VocalAnalysisResponse(ResponseModel):
    sibilance_timeline: list[SibilanceSample]
    breath_markers: list[BreathMarker]
    plosive_markers: list[PlosiveMarker]
    presence_score: float          # 0..1
    clarity_score: float           # 0..1
    warmth_score: float            # 0..1
    formant_balance: dict[str, float]
    window_sec: float
    dominant_sibilance_hz: float = 0.0  # 0.0 = no sibilant frames detected
    sii_timeline: list[SiiSample] = []
    vocal_sii_score: float = 1.0


class LyricsRequest(RequestModel):
    file_path: str
    # Allowlisted (audit SEC-02): faster-whisper accepts either a known model
    # name or an arbitrary HuggingFace repo ID/local path — an unvalidated
    # string here would let a client make this service download and load any
    # HF repo, an unsafe-deserialization surface applying the same reasoning
    # as the Demucs checkpoint verification (SEC-03).
    model_size: Literal["tiny", "base", "small", "medium", "large-v3"] = "base"


class LyricWordResult(ResponseModel):
    word: str
    start_sec: float
    end_sec: float
    presence_share: float      # 0..1, local vocal-presence-band energy share
    masking_ratio: float       # presence_share / track_avg_presence_share
    likely_masked: bool
    probability: float | None = None   # Whisper word confidence (AUDIT-080)


class LyricAnalysisResponse(ResponseModel):
    words: list[LyricWordResult]
    track_avg_presence_share: float
    flagged_count: int


# --- Fix-at-Source Triage (/analyze/source_triage) ---

class SourceTriageRequest(RequestModel):
    file_path: str
    #: What the file is (CON-7): "mix" for a session's mix or master (no
    #: capture checks - the full intake grades it this way, and the same
    #: request reads the intake's cached result), "auto" to detect it.
    material: Literal["auto", "mix", "master", "single_source", "stem"] = "auto"


class FindingConfidenceBasis(ResponseModel):
    kind: str                       # "calibrated" | "heuristic"
    n: int | None = None
    precision: float | None = None
    recall: float | None = None
    probability: float | None = None
    corpus_sha256: str | None = None


class SourceTriageFinding(ResponseModel):
    code: str          # machine key: clipping | dropouts | comb_filtering | ...
    title: str
    detail: str        # human-readable explanation + what to do
    category: str      # "rerecord" | "mitigable" | "fixable" | "info"
    severity: float    # 0..1
    # CE-TRU-1: "calibrated" (n, precision, recall, probability) when a table
    # built from a labelled corpus covers this code, else "heuristic".
    confidence_basis: FindingConfidenceBasis | None = None


class SourceTriageResponse(ResponseModel):
    file_path: str
    duration_sec: float
    findings: list[SourceTriageFinding]
    verdict: str       # "clean" | "fix in the box" | "mitigate before mixing" | "re-record recommended"


# --- Arrangement Element Analyzer (/analyze/arrangement) ---

class ArrangementRequest(RequestModel):
    vocals_path: str
    drums_path: str
    bass_path: str
    other_path: str
    window_sec: float = 8.0


class ArrangementWindow(ResponseModel):
    start_sec: float
    end_sec: float
    shares: dict[str, float]       # element -> 0..1 energy share
    present: dict[str, bool]       # element -> carries a meaningful share
    silent: bool


class ArrangementFlag(ResponseModel):
    start_sec: float
    end_sec: float
    code: str                      # overcrowded | no_foundation | no_lead
    message: str


class ArrangementResponse(ResponseModel):
    window_sec: float
    windows: list[ArrangementWindow]
    flags: list[ArrangementFlag]
    element_presence: dict[str, float]   # element -> fraction of windows present


# --- Smart Gain Rider (/analyze/gain_rider) ---

class GainRiderRequest(RequestModel):
    file_path: str
    target_rms_db: float = -18.0
    max_gain_db: float = 12.0
    render: bool = False           # True: also write the ridden file
    # UIX-11: where to write the render (allowed roots, never the source);
    # absent = <file>_gainride.wav beside the source, as before.
    output_path: str | None = None


class GainRiderPhrase(ResponseModel):
    start_sec: float
    end_sec: float
    rms_db: float                  # measured phrase level
    gain_db: float                 # applied correction toward target


class GainRiderResponse(ResponseModel):
    file_path: str
    target_rms_db: float
    noise_floor_db: float
    window_sec: float              # envelope sample spacing
    phrases: list[GainRiderPhrase]
    envelope_db: list[float]       # per-window gain for UI overlay
    rendered_path: str | None = None
    peak_limited_db: float | None = None   # set if the render needed a safety trim


# --- Depth & Dimension Mapper (/analyze/depth) ---

class DepthRequest(RequestModel):
    stems: dict[str, str] = Field(max_length=16)   # stem name -> file path


class StemDepth(ResponseModel):
    stem: str
    depth: float                   # 0 = front, 1 = back wall
    wetness: float                 # gap-tail energy cue
    darkness: float                # HF rolloff cue
    softness: float                # transient bluntness cue


class DepthResponse(ResponseModel):
    stems: list[StemDepth]
    suggestions: list[str]


# --- Compression X-Ray (/analyze/gain_xray) ---

class GainXrayRequest(RequestModel):
    original_path: str
    processed_path: str


class GainXrayEvent(ResponseModel):
    start_sec: float
    end_sec: float
    code: str                      # pumping | over_compression
    message: str


class GainXrayResponse(ResponseModel):
    window_sec: float              # curve sample spacing
    gain_curve_db: list[float]     # dynamic gain (static offset removed)
    brightness_curve_db: list[float] = []  # tonal delta (high/low band ratio, processed - original)
    events: list[GainXrayEvent]
    avg_gain_db: float             # mean reduction where active (negative)
    max_reduction_db: float
    static_offset_db: float = 0.0  # makeup gain / normalization component
    pumping_score: float
    verdict: str


# --- Album Assembly (/analyze/album) ---

class AlbumRequest(RequestModel):
    track_paths: list[str] = Field(max_length=32)   # album order


class AlbumTrack(ResponseModel):
    path: str
    lufs: float
    crest_db: float
    tilt_db_per_octave: float
    duration_sec: float


class AlbumFlag(ResponseModel):
    track_index: int               # seam between track_index and track_index+1 (1-based msg)
    code: str                      # loudness_jump | tonal_shift | density_shift
    message: str


class AlbumResponse(ResponseModel):
    tracks: list[AlbumTrack]
    flags: list[AlbumFlag]
    lufs_spread: float
    verdict: str


# --- Pan Energy Balance (/analyze/pan_balance) ---

class PanBalanceRequest(RequestModel):
    file_path: str
    window_sec: float = 1.0


class PanBandSummary(ResponseModel):
    band: str
    low_hz: float
    high_hz: float
    bias: float                    # -1 hard left .. +1 hard right (whole track)
    width: float                   # side share 0 (mono) .. ~0.5 (M==S)
    side_share: float | None = None  # S/(M+S) energy share (diagnostic; panels read width)


class PanBalanceResponse(ResponseModel):
    window_sec: float
    bands: list[str]
    bias_curves: dict[str, list[float]]   # band -> per-window bias
    band_summary: list[PanBandSummary]
    warnings: list[str]


# --- Comping Assistant (/analyze/comping) ---

class CompingRequest(RequestModel):
    take_paths: list[str] = Field(max_length=16)   # >= 2 aligned takes
    render: bool = False           # True: also write the comp
    # Render destination (UIX-11; validated like every render path). None:
    # <take1>_comp.wav beside the first take.
    output_path: str | None = None
    # The mix's tuning reference (concert A, Hz) the pitch score measures
    # against; None = A=440 (THY-02).
    reference_hz: float | None = None


class CompingTakeScore(ResponseModel):
    take_index: int
    pitch_score: float             # 0..1: intonation (deviation from the grid + wobble)
    level_score: float             # 0..1: closeness to the takes' median level
    tone_score: float              # 0..1: timbre consistency
    total: float
    # Duration-weighted mean |cents| off the nearest semitone of the grid and
    # mean detrended within-note wobble; None when no notes were measurable.
    mean_abs_cents: float | None = None
    wobble_cents: float | None = None


class CompingPhrase(ResponseModel):
    start_sec: float
    end_sec: float
    takes: list[CompingTakeScore]
    best_take: int


class CompingResponse(ResponseModel):
    sample_rate: int
    take_count: int
    phrases: list[CompingPhrase]
    rendered_path: str | None = None


# --- Signal Integrity (/analyze/signal_integrity) ---

class SignalIntegrityRequest(RequestModel):
    file_path: str


# --- Track Quick Stats (/analyze/quick_stats) ---

class QuickStatsRequest(RequestModel):
    file_path: str


class FullIntakeRequest(RequestModel):
    """Initial intake context.

    Genre inputs are deliberately intent fields, not labels inferred from the
    audio.  A detected candidate remains unconfirmed until the user confirms
    it or supplies a reference track.
    """
    file_path: str
    stated_genre_id: str = ""
    reference_path: str = ""
    use_genre_targets: bool = True
    confirm_detected_genre: bool = False
    selection_mode: Literal["detected", "stated", "reference", "none"] = "detected"
    user_metadata: dict[str, str] = {}
    # Optional separated stems. When all four are present, the orchestrator
    # folds stem-dependent analyses into its heavy stage rather than making
    # panels independently re-run them.
    stems: dict[str, str] = {}


class CleanMetadataRequest(RequestModel):
    # Delivery-metadata cleaning (/metadata/clean, 2026-11-07 reframe of
    # "Strip Metadata"): removes workflow / personal tags only; identity,
    # rights and every AI-provenance / disclosure tag are kept.
    file_path: str
    # Empty → "<source>_clean.<ext>" beside the source file.
    output_path: str = ""


class DeepHumanizeRequest(RequestModel):
    # Per-stem paths (e.g. {"vocals": "...", "drums": "..."}) and per-stem
    # humanize intensity 0..1. Stems missing an intensity use `default_intensity`.
    stems: dict[str, str] = Field(max_length=16)
    intensities: dict[str, float] = {}
    default_intensity: float = 0.5
    # Empty → "<first-stem-dir>/deep_humanized.wav".
    output_path: str = ""
    wow_rate_hz: float = 0.3
    depth_ms: float = 4.0
    level_var_db: float = 0.8
    noise_floor_db: float = -72.0
    # Optional per-stem groove overrides, keyed by stem name (same keys as
    # `stems`). A stem absent from an override dict falls back to that
    # parameter's global scalar above - existing single-groove callers are
    # unaffected. Lets Deep Humanize give e.g. drums a faster wow rate than
    # vocals instead of one groove shape for every stem.
    wow_rate_overrides: dict[str, float] = {}
    depth_overrides: dict[str, float] = {}
    level_var_overrides: dict[str, float] = {}
    noise_floor_overrides: dict[str, float] = {}


class AnalogFeelRequest(RequestModel):
    # Analog Feel (/humanize/analog_feel, 2026-11-07 reframe of the closed-loop
    # "targeted humanize"): a creative single-pass render. The user picks the
    # character and the amount; nothing is scanned and no detector score is
    # targeted.
    file_path: str
    output_path: str = ""            # empty -> "<source>_analog.wav" beside the source
    amount: float = Field(default=0.4, ge=0.0, le=1.0)
    tape_drift: bool = True          # wow/flutter: micro-timing + micro-pitch drift
    fader_ride: bool = True          # a slow random level ride, like a hand on the fader
    room_tone: bool = True           # a breathing low-level floor instead of digital silence
    seed: int = 1337
    # Optional stems: tape drift runs per stem (decorrelated), then the sum
    # gets the fader ride and room tone.
    stems: dict[str, str] | None = Field(default=None, max_length=16)


# --- Stem Taxonomy (/analyze/stem_taxonomy) ---

class StemTaxonomyRequest(RequestModel):
    vocals_path: str
    drums_path: str
    bass_path: str
    other_path: str
    # Optional genre id (e.g. "trap"): when set, the response includes
    # per-bucket target_share_pct derived from the genre profile (spec §8).
    genre_id: str = ""


# --- Pitch Accuracy (/analyze/pitch_accuracy) ---

class PitchAccuracyRequest(RequestModel):
    file_path: str
    fmin: float = Field(default=70.0, ge=20.0, le=2000.0)
    fmax: float = Field(default=1200.0, ge=40.0, le=4000.0)
    # Measurement grid (concert A). None -> A=440. Pass the mix's tuning
    # reference to judge a take against the track it sits in.
    reference_hz: float | None = Field(default=None, ge=400.0, le=480.0)


# --- MIDI Variation ---

class MidiVariationRequest(RequestModel):
    file_path: str
    timing_variance: float = Field(default=0.02, ge=0.0, le=0.25)       # seconds (std dev)
    velocity_variance: float = Field(default=10.0, ge=0.0, le=64.0)     # MIDI velocity (std dev)
    pitch_variation_prob: float = Field(default=0.05, ge=0.0, le=1.0)
    stem_type: str = "auto"
    output_path: str = ""
    # Concert A for the pitched MIDI extraction (AUDIT-016); None = 440.
    reference_hz: float | None = Field(default=None, ge=400.0, le=480.0)


# --- Pitch note-grid editor (/analyze/pitch_curve, /render/pitch_correct) ---

class PitchCurveRequest(RequestModel):
    file_path: str
    fmin: float = Field(default=70.0, ge=20.0, le=2000.0)
    fmax: float = Field(default=1200.0, ge=40.0, le=4000.0)
    # Snap grid (concert A). None -> A=440; pass the mix's tuning reference
    # (or e.g. 432) to correct onto that grid instead.
    reference_hz: float | None = Field(default=None, ge=400.0, le=480.0)


class PitchCurveNote(ResponseModel):
    id: int
    start_sec: float
    end_sec: float
    midi: float                    # mean fractional MIDI, relative to reference_hz
    target_midi: int               # nearest semitone on the reference_hz grid


class PitchCurveResponse(ResponseModel):
    duration_sec: float
    hop_sec: float                 # spacing of f0_midi samples
    f0_midi: list[float | None]    # None = unvoiced; relative to reference_hz
    notes: list[PitchCurveNote]
    # The grid midi / f0_midi / target_midi are on (MIDI 69 == reference_hz):
    # the requested reference, A=440 by default - so target_midi - midi is
    # the correction onto that grid. Absolute A=440 MIDI is
    # midi + 12*log2(reference_hz/440).
    reference_hz: float = 440.0
    # The take's own tuning (additive): its estimated concert-A, that
    # reference's offset from A=440 in cents, and whether it sits > 10 cents
    # off the grid above.
    measurement_reference_hz: float | None = None
    detected_reference_hz: float | None = None
    reference_offset_cents: float | None = None
    reference_offset_warning: bool | None = None


class PitchCorrection(RequestModel):
    start_sec: float
    end_sec: float
    shift_semitones: float


class PitchCorrectRequest(RequestModel):
    file_path: str
    corrections: list[PitchCorrection]
    crossfade_ms: float = 10.0
    output_path: str | None = None  # None -> "<source>_pitchcorrected.wav" beside the source


class PitchCorrectResponse(ResponseModel):
    rendered_path: str
    applied: int


# --- Doubles / ad-lib timing alignment (/analyze/align_takes) ---

class AlignTakesRequest(RequestModel):
    reference_path: str
    take_paths: list[str]          # 1..8
    mode: str = "align"            # "align" | "quantize"
    bpm: float | None = None       # quantize only; None -> estimated from reference
    grid_division: int = 16        # 4, 8, 16, 32
    strength: float = 1.0          # 0..1
    max_shift_ms: float = 80.0
    render: bool = True
    # Render destinations, one per take (UIX-11; validated like every render
    # path). None: <take>_<aligned|quantized>.wav beside each take.
    output_paths: list[str] | None = None


class AlignSegment(ResponseModel):
    start_sec: float
    end_sec: float
    shift_ms: float                # positive = moved later


class AlignedTake(ResponseModel):
    path: str
    segments: list[AlignSegment]
    mean_abs_offset_ms_before: float
    mean_abs_offset_ms_after: float
    rendered_path: str | None = None


class AlignTakesResponse(ResponseModel):
    mode: str
    bpm: float | None = None
    takes: list[AlignedTake]


# --- Per-word clip gain (/render/word_clip_gain) ---

class WordGainRegion(RequestModel):
    start_sec: float
    end_sec: float
    gain_db: float


class WordClipGainRequest(RequestModel):
    file_path: str
    regions: list[WordGainRegion]
    fade_ms: float = 8.0
    output_path: str | None = None  # None -> "<source>_wordgain.wav" beside the source


class WordClipGainResponse(ResponseModel):
    rendered_path: str
    regions_applied: int
    peak_dbfs: float
