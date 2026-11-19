#pragma once
#include <khensu/hooks/HookResult.h>
#include <khensu/memorability/MemorabilityResult.h>
#include <khensu/memorability/StructuralFlowResult.h>
#include <khensu/heaviness/HeavinessResult.h>
#include <khensu/recommendations/Recommendation.h>
#include <khensu/analysis/SectionAnalysis.h>
#include <khensu/analysis/ChordAnalysis.h>
#include <khensu/analysis/MaskingSummary.h>
#include <khensu/analysis/AnalysisInputs.h>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>
#include <map>

namespace khensu {

/// Aggregated result from a full analysis run for one audio file.
///
/// C++ fast-analysis fields are populated by SP-03 analyzers (running in WorkerThread).
/// python_data fields are populated by the IPC bridge in SP-05 after the Python service responds.
/// Hook/memorability/heaviness typed structs replace python_data JSON strings in SP-06.
/// Recommendations are populated by the RecommendationEngine in SP-07.
///
/// THREAD_CONTEXT(WorkerThread | UIThread) - contains std::string/std::vector members.
struct AnalysisResult {
    // --- Identity ---
    std::string id; // UUID v4
    std::string session_id;
    std::string file_id;
    std::string computed_at; // ISO 8601 UTC
    bool is_partial = false; // True while Python deep analysis is still in flight.

    // Set when the analysis job could not produce a result at all (an
    // exception during the run, or every file in the session failed to
    // decode/analyze). Callers should show error_message and stop waiting
    // rather than treat this as a normal (if empty) result.
    bool analysis_failed = false;
    std::string error_message;

    /// Optional inputs this analysis asked for and did not get (REF-1): the
    /// engine did not answer in time, or a request failed. A mask of
    /// analysis_inputs::k* bits. The result is still final (is_partial is
    /// false), so a reader that needs to know whether an absent finding was
    /// measured checks inputs_available(), and one that needs to know
    /// whether a re-run could add something checks this.
    std::uint32_t inputs_failed = 0;

    // The sample rate the fast C++ analyzers actually measured from (see
    // AudioAnalyzer::analyze). Lets UI components that map FFT bin index -> Hz
    // (e.g. HeatSpectrumComponent) do so correctly instead of assuming 44100
    // (kimi audit P1 #36 / UI-13). 0.0 means "not yet measured".
    double sample_rate = 0.0;

    // --- Measurement trust map (pipeline #5) ---
    /// Per-measurement reliability from the service's
    /// /analyze/measurement_quality endpoint, keyed by the service's metric
    /// names ("loudness_lufs", "crest_factor_db", "true_peak_dbtp", "bpm",
    /// "key"). Populated best-effort during deep analysis (same carrier
    /// pattern as sample_rate above); empty on fast-only results. Consumed by
    /// RecommendationEngine to down-weight recommendations built on
    /// tentative/unreliable measurements.
    struct MeasurementTrust {
        float confidence = 0.0f;  ///< deterministic measurement precision, 0-1
        std::string quality_flag; ///< "reliable" | "tentative" | "unreliable"
        std::string note;         ///< human-readable caveat, e.g. "(short excerpt)"
    };
    std::map<std::string, MeasurementTrust> measurement_trust;

    // --- Channel layout (audit AQ-01) ---
    /// The resolved channel layout this result was measured from. Carried on
    /// the result so the service, UI, and render paths all read one
    /// authoritative descriptor instead of re-deriving policy from a raw
    /// channel count. An empty layout_name means "not measured".
    struct ChannelLayoutData {
        std::string layout_name;   ///< "Mono" / "Stereo" / "5.1" / "7.1"
        int channel_count = 0;
        bool lfe_excluded = false; ///< true when the layout has an LFE channel
    } channel_layout;

    // --- Loudness (ITU-R BS.1770-4) ---
    struct LoudnessData {
        float lufs_integrated = -99.0f;     // Integrated LUFS
        float lufs_short_term_max = -99.0f; // Max short-term LUFS (3s window)
        float lra = 0.0f;                   // Loudness Range (LU)
        /// False when the material is too short (or too quiet) for at least
        /// two gated 3 s short-term windows - lra is then 0 but MEANINGLESS,
        /// and must not drive "too compressed" advice or scoring.
        bool lra_measured = true;
        float plr = 0.0f;                   // Peak-to-Loudness Ratio (dB)
        float true_peak_dbtp = -99.0f;      // Max true peak (dBTP)
    } loudness;

    // --- Spectral ---
    struct SpectralData {
        std::vector<float> rms_per_octave_db;      // 10 octave bands, 31Hz-16kHz
        std::vector<float> rms_mid_per_octave_db;  // 10 octave bands for Mid (mono)
        std::vector<float> rms_side_per_octave_db; // 10 octave bands for Side (difference)
        std::vector<float> fft_mid_magnitudes;     // Full FFT magnitudes for Mid (~2048 bins)
        std::vector<float> fft_side_magnitudes;    // Full FFT magnitudes for Side (~2048 bins)
        float centroid_hz = 0.0f;
        float balance_low = 0.0f;  // 0-1 energy share below 250Hz
        float balance_mid = 0.0f;  // 0-1 energy share 250Hz-4kHz
        float balance_high = 0.0f; // 0-1 energy share above 4kHz
        // Least-squares slope of rms_per_octave_db against log2(bandCenterHz),
        // in dB/octave. Negative = darker/rolled-off highs (typical program
        // material, shoegaze/lo-fi/ambient); near zero or positive = bright,
        // hyperpop-style top end. Lets genre targets score overall tonal
        // balance shape, not just a single centroid_hz scalar.
        float tilt_db_per_octave = 0.0f;
    } spectral;

    // --- Dynamics ---
    struct DynamicsData {
        float crest_factor_db = 0.0f;
        float dynamic_range_db = 0.0f; // EBU DR value
        float peak_to_rms_db = 0.0f;
    } dynamics;

    // --- Stereo ---
    struct StereoData {
        float width = 0.0f;       // 2*S^2/(M^2+S^2) clamped: 0 = mono, 1 = uncorrelated L/R
        float correlation = 0.0f; // -1 = anti-phase, 0 = uncorrelated, 1 = identical
        float ms_balance = 0.0f;  // Mid/Side energy ratio; 0 = balanced
    } stereo;

    // --- Phase ---
    struct PhaseData {
        /// One phase threshold for every consumer (CORE-06): the analyzer's
        /// polarity flag, StereoRecommender's phase_issue card and
        /// StrengthRecommender's "phase clean" note all compare the L/R
        /// correlation against this value, so they can never contradict.
        static constexpr float kPhaseIssueCorrelation = 0.0f;

        float overall_correlation = 0.0f;
        bool has_polarity_issue = false; ///< overall_correlation < kPhaseIssueCorrelation
        /// Mid power below 0.6x the summed L+R power: the stereo pair carries
        /// enough anti-phase energy that the mono fold-down loses level
        /// (CORE-09). This is an anti-correlation test, not a comb-filter
        /// detector - nothing in the fast pass measures delay-induced notches.
        bool has_anti_phase_energy = false;
        /// DEPRECATED alias of has_anti_phase_energy, kept so existing readers
        /// compile; the name overclaimed (it never detected comb filtering).
        /// Read has_anti_phase_energy instead.
        bool has_comb_filtering = false;
    } phase;

    // --- Artifact Detection ---
    struct ArtifactData {
        bool has_clipping = false;
        int clipping_frame_count = 0;
        float noise_floor_db = -90.0f;
        /// How long the passages the floor was read from last (s), and how
        /// far their median sits above the floor (dB). A steady hiss or hum
        /// fills its gaps at one level (spread of a few dB); a breakdown or a
        /// fade is music, spread over tens of dB (MEAS-4).
        float noise_quiet_seconds = 0.0f;
        float noise_floor_spread_db = 0.0f;
        // No AI flag here (FIX-13): the fast C++ pass has no calibrated
        // detector (AUDIT-010). AI detection is the service's, on the AI
        // Detection screen, with its own evidence classes.
        /// Uncalibrated, informational only: share of samples where the mono
        /// signal's slope reverses direction (0..1). Tracks spectral slope and
        /// sample rate, not generation - white noise ~0.67, pink ~0.58, a
        /// typical mix ~0.53, a 100 Hz sine ~0.004. Never a probability of AI.
        float sample_reversal_rate = 0.0f;
        /// Mean of the mono signal over the whole file (signed, full scale
        /// 1.0): DC offset. A mix centred on zero reads about 0 (MEAS-4).
        float dc_offset = 0.0f;
        /// The offset holds through the file: the 10th and 90th percentiles of
        /// its one-second means share dc_offset's sign. A converter's offset
        /// is steady; a whole-file mean built from swinging sub-sonic content
        /// (half the released tracks the mean alone flagged) is not - MEAS-4.
        /// True for a file under three seconds (the mean is all there is).
        bool dc_offset_steady = true;
        /// The service's source-triage threshold (source_triage._check_dc_offset).
        static constexpr float kDcOffsetThreshold = 0.002f;
    } artifacts;

    // --- Mono compatibility (SP-16C) ---
    struct MonoCompatData {
        std::map<std::string, float> per_band_correlation; ///< band_name -> correlation -1..1
        float low_end_stereo_pct = 0.0f;                   ///< 0-100: percentage of sub+bass energy that is stereo

        static constexpr const char* kBandSub = "sub";
        static constexpr const char* kBandBass = "bass";
        static constexpr const char* kBandMidrange = "midrange";
        static constexpr const char* kBandUpperMid = "upper_mid";
        static constexpr const char* kBandAir = "air";
    };

    // --- Translation risk (SP-16C) ---
    struct TranslationRiskData {
        std::map<std::string, float> per_environment; ///< env_name -> risk 0..1
        float score = 0.0f;                           ///< aggregate mean risk 0..1

        static constexpr const char* kEnvStudio = "studio";
        static constexpr const char* kEnvEarbuds = "earbuds";
        static constexpr const char* kEnvPhone = "phone";
        static constexpr const char* kEnvCar = "car";
        static constexpr const char* kEnvClub = "club";
    };

    // --- Harshness (SP-16C) ---
    struct HarshnessData {
        float score = 0.0f;           ///< 0-1
        std::optional<float> peak_hz; ///< Dominant harsh frequency; nullopt if score <= 0.2
        std::optional<float> peak_db; ///< Level of dominant peak; nullopt if score <= 0.2
    };

    // --- Resonance hotspots (SP-16C) ---
    struct ResonancePeak {
        float hz = 0.0f;
        float db = 0.0f;         ///< absolute magnitude in dB at this frequency
        float prominence = 0.0f; ///< prominence above surrounding spectrum (dB)
    };

    struct ResonanceData {
        std::vector<ResonancePeak> hotspots; ///< Top-5 resonance peaks by prominence
    };

    // --- Mid-side frequency imbalance (from /analyze/extended) ---
    struct MidSideImbalanceData {
        float side_bright_ratio = 1.0f;       ///< side/mid energy above 4 kHz (>1.5 = too bright)
        float side_heavy_ratio = 0.0f;        ///< side/mid energy below 100 Hz (>0.3 = sub too wide)
        float brightness_imbalance_db = 0.0f; ///< dB(side) - dB(mid) in high band
        float heaviness_imbalance_db = 0.0f;  ///< dB(side) - dB(mid) in low band
        float imbalance_severity = 0.0f;      ///< 0-1 composite severity
        std::string warning;                  ///< Human-readable warning, empty if none
    };

    // --- Extended analysis (SP-16C, populated from /analyze/extended) ---
    // All four are absent (nullopt) until an extended analysis run completes.
    std::optional<MonoCompatData> mono_compat;
    std::optional<TranslationRiskData> translation_risk;
    std::optional<HarshnessData> harshness;
    std::optional<ResonanceData> resonance;
    std::optional<MidSideImbalanceData> mid_side_imbalance;

    // --- Emotional-arc tracking (populated from /analyze/extended) ---
    // Time-windowed companion to PythonData::energy_detected/mood_detected -
    // those two are single whole-track values; this shows how energy/mood
    // actually move across the song.
    struct EmotionalArcPoint {
        int window_index = 0;
        float progress_fraction = 0.0f; ///< 0..1 song position
        float energy_0_100 = 0.0f;
        std::string mood;
    };
    struct EmotionalArcData {
        std::vector<EmotionalArcPoint> points;
        std::vector<std::pair<float, float>> flat_sections; ///< progress-fraction ranges
        float max_energy_delta = 0.0f;
        /// NEW-9: the CLAP mood probe's whole-track tags (empty when it did not run).
        std::vector<std::string> track_mood_tags;
    };
    std::optional<EmotionalArcData> emotional_arc;

    // --- Genre-blend detection (populated from /analyze/extended) ---
    // Top-N genre/subgenre relative ranks, supplementing PythonData's single
    // genre_detected/subgenre_detected label. Sorted by rank
    // descending; always has at least one entry when present.
    struct GenreBlendEntry {
        std::string genre;
        std::string subgenre;
        float rank_score = 0.0f; ///< heuristic relative rank; not probability/confidence
    };
    std::vector<GenreBlendEntry> genre_blend;
    // Provenance and safety metadata for genre-relative recommendations.
    std::string genre_target_source;
    std::string genre_confirmation_status;
    std::string genre_abstained_reason;
    bool genre_relative_action_allowed = false;
    bool genre_is_hybrid = false;

    // --- Translation prediction (populated from /analyze/extended) ---
    // Deeper companion to TranslationRiskData: an actual per-device
    // frequency-response simulation + masking-collision re-analysis, rather
    // than a hand-tuned heuristic risk score.
    struct TranslationCollision {
        float band_a_lo_hz = 0.0f, band_a_hi_hz = 0.0f;
        float band_b_lo_hz = 0.0f, band_b_hi_hz = 0.0f;
        float collision_score = 0.0f; ///< 0..1
        float increase = 0.0f;        ///< delta vs. flat/studio reference
    };
    struct TranslationProfileScore {
        float degradation_score = 0.0f; ///< 0..1
        std::vector<TranslationCollision> new_masking_collisions;
    };
    struct TranslationPredictionData {
        std::map<std::string, TranslationProfileScore> per_profile; ///< device -> score
    };
    std::optional<TranslationPredictionData> translation_prediction;

    // --- Chart/reference benchmarking (populated from /analyze/extended) ---
    // Percentile rank against a per-genre reference-measurement library,
    // absent when the detected genre has no library entry (see
    // benchmark.py's module docstring re: bootstrap/synthetic data source).
    struct BenchmarkData {
        std::string genre_id;
        std::map<std::string, float> percentile_by_metric; ///< metric -> 0..100
        std::string competitive_summary;
        int sample_size = 0;
        bool is_bootstrap_data = false;
    };
    std::optional<BenchmarkData> benchmark;

    // --- Stereo-field-over-time (populated from /analyze/extended) ---
    // Time-windowed companion to StereoData/MonoCompatData: those are
    // whole-track snapshots; this shows how correlation/width/mono-%
    // actually move across the song.
    struct StereoFieldPoint {
        int window_index = 0;
        float progress_fraction = 0.0f;
        float correlation = 0.0f;       ///< -1..1
        float width = 0.0f;             ///< 0..1, M/S ratio (see extended_analyzer.analyze_stereo_field_over_time)
        float sub_bass_mono_pct = 0.0f; ///< 0..100
    };
    struct StereoFieldTimeline {
        std::vector<StereoFieldPoint> points;
    };
    std::optional<StereoFieldTimeline> stereo_field_timeline;

    // --- Lyric-aware mixing hints (populated on-demand via /analyze/lyrics) ---
    // Not part of the main analysis pipeline - transcription is slow and
    // needs the optional faster-whisper dependency, so this is only
    // populated once the user explicitly runs it (same pattern as
    // `masking` below: absent until requested, not auto-run on every file).
    struct LyricWordResult {
        std::string word;
        float start_sec = 0.0f;
        float end_sec = 0.0f;
        float presence_share = 0.0f; ///< 0..1, local vocal-presence-band energy share
        float masking_ratio = 0.0f;  ///< presence_share / track_avg_presence_share
        bool likely_masked = false;
    };
    struct LyricIntelligibilityData {
        std::vector<LyricWordResult> words;
        float track_avg_presence_share = 0.0f;
        int flagged_count = 0;
    };
    std::optional<LyricIntelligibilityData> lyric_intelligibility;

    // Populated once the user runs "Analyze Masking" in StemMaskingPanel
    // (stem-pair frequency-collision scores) - absent until then, since it
    // requires already-separated stems and isn't part of the main pipeline.
    std::optional<MaskingSummary> masking;

    // --- Drum analysis (populated from /analyze/drums endpoint) ---
    /// One detected kick/snare hit. router_audio.py's DrumEventResult already
    /// carries this per-hit detail (onset/attack/body/fundamental); it was
    /// computed on every run but the C++ bridge only ever extracted the
    /// aggregate DrumData scalars below, silently dropping the event list -
    /// so no piano-roll-style groove timeline was possible even though the
    /// data existed end-to-end. See DrumPatternPanel.
    struct DrumHitEvent {
        float onset_sec = 0.0f;
        float attack_ms = 0.0f; ///< lower = sharper transient
        float body_db = 0.0f;   ///< hit loudness
        float fund_hz = 0.0f;   ///< fundamental frequency of the hit
    };
    struct DrumData {
        float kick_timing_regularity = 0.0f;  ///< 0-1; 1 = metronomic kick
        float snare_timing_regularity = 0.0f; ///< 0-1; 1 = metronomic snare
        float kick_avg_bpm = 0.0f;            ///< Kick hit rate (hits/min), not tempo
        float transient_sharpness = 0.0f;     ///< 0-1; fraction of events with attack < 10 ms
        bool kick_bass_clash = false;         ///< Kick fundamental within 10 Hz of bass
        float kick_bass_proximity_hz = 0.0f;  ///< |kick_fund_hz - bass_fund_hz|
        std::vector<DrumHitEvent> kick_events;
        std::vector<DrumHitEvent> snare_events;
    };
    std::optional<DrumData> drums;

    // --- Section-level and reference analysis (SP-16D) ---
    // Populated from /analyze/sections and /analyze/reference endpoints.
    std::optional<SectionAnalysis> section_analysis;
    std::optional<ReferenceDeltas> reference_deltas;

    // --- Chord progression analysis (SP-23) ---
    // Populated from /analyze/chords endpoint after deep analysis completes.
    std::optional<ChordAnalysis> chord_analysis;

    // --- Python Deep Analysis (raw JSON from local service, SP-05) ---
    // Replaced by typed structs in SP-06 once HookResult/MemorabilityResult/HeavinessResult exist.
    struct PythonData {
        float bpm_detected = 0.0f;
        float bpm_confidence = 0.0f;
        std::string pitch_key;
        float pitch_confidence = 0.0f;
        std::string genre_detected;
        std::string subgenre_detected;
        float genre_confidence = 0.0f;
        float energy_detected = 0.0f;    // 0-100, perceptual energy heuristic
        std::string mood_detected;       // valence/arousal heuristic label
        std::string beat_times_json;     // Raw JSON array of beat times (seconds)
        std::string downbeat_times_json; // Raw JSON array of downbeat times; "" in quick mode
        std::string sections_json;       // Raw JSON array of {start_sec, label}
        std::string pitch_notes_json;    // Raw JSON array of pitch notes; "" in quick mode
        // Meter + key relations (advanced mode only; empty/0 in quick mode,
        // which assumes 4/4). time_signature is "4/4", "3/4" or "6/8"; a 4/4
        // reading with meter_confidence <= 0.2 is the fallback, not a
        // detection. felt_bpm is the dotted-quarter tempo of a 6/8 tracked at
        // the eighth-note pulse, 0 otherwise. camelot is e.g. "8A".
        std::string time_signature;
        float meter_confidence = 0.0f;
        float felt_bpm = 0.0f;
        std::string camelot;
        std::string relative_key;
        HookResult hooks;
        MemorabilityResult memorability;
        StructuralFlowResult structural_flow;
        HeavinessResult heaviness;
        float skewness = 0.0f;
        float peak_asymmetry_ratio = 1.0f;
        bool is_asymmetric = false;
        // More music-theory identity (advanced /analyze + /analyze/extended).
        // Empty / negative = the service did not report it.
        int circle_of_fifths_index = -1;     ///< 0..11
        std::string parallel_key;            ///< e.g. "C minor"
    } python_data;

    // --- Recommendations (populated by SP-07 RecommendationEngine) ---
    std::vector<Recommendation> recommendations;

    // --- Batch Processing (Item #29) ---
    std::vector<AnalysisResult> batch_results;

    bool has_fast_analysis() const noexcept { return loudness.lufs_integrated > -98.0f; }
    /// Set when the deep (Python) pass actually ran. BPM alone is not a valid
    /// signal: beatless material (ambient, drones, spoken word) legitimately
    /// has no tempo, so it was treated as "never analyzed" - never served from
    /// cache and re-run on every open.
    bool python_analysis_ran = false;
    bool has_python_analysis() const noexcept { return python_analysis_ran || python_data.bpm_detected > 0.0f; }
    bool has_recommendations() const noexcept { return !recommendations.empty(); }

    /// The optional inputs this result holds (REF-1), as analysis_inputs::k*
    /// bits. Derived from the fields themselves, so it cannot disagree with
    /// them. A recommender only raises a finding when its input is here, so
    /// a finding absent from a result that lacks its input was not measured.
    std::uint32_t inputs_available() const noexcept
    {
        namespace in = analysis_inputs;
        std::uint32_t m = in::kNone;
        if (has_python_analysis()) m |= in::kDeep;
        if (harshness.has_value()) m |= in::kHarshness;
        if (mono_compat.has_value()) m |= in::kMonoCompat;
        if (translation_risk.has_value()) m |= in::kTranslationRisk;
        if (resonance.has_value()) m |= in::kResonance;
        if (mid_side_imbalance.has_value()) m |= in::kMidSide;
        if (section_analysis.has_value()) m |= in::kSections;
        if (drums.has_value()) m |= in::kDrums;
        if (masking.has_value()) m |= in::kMasking;
        if (reference_deltas.has_value()) m |= in::kReference;
        if (chord_analysis.has_value()) m |= in::kChords;
        if (!measurement_trust.empty()) m |= in::kTrust;
        return m;
    }
};

} // namespace khensu
