#pragma once
#include <khensu/ipc/BridgeTransport.h>

namespace khensu::ipc {

/// Analysis routes: file analyses, stems, reference, export targets (ENG-4). Implemented in
/// src/ipc/HttpServiceBridge_Analysis.cpp. Usable on its own (`AnalysisApi{}.method(...)`)
/// or through HttpServiceBridge, which derives from every domain client.
class AnalysisApi : public virtual BridgeTransport {
  public:
    AnalysisApi() = default;
    explicit AnalysisApi(std::string base_url, int timeout_ms = kDefaultTimeoutMs,
                         int retry_delay_ms = kDefaultRetryDelayMs)
        : BridgeTransport(std::move(base_url), timeout_ms, retry_delay_ms)
    {}

    // Non-interface extensions - call directly when concrete type is known.
    khensu::Result<khensu::AnalysisResult::DrumData, BridgeError> analyzeDrums(const std::string& file_path);

    /// POST /analyze/loudness_curve - short-term (3 s window / 1 s hop)
    /// K-weighted LUFS series, the same underlying measurement
    /// loudness_range() already computes for LRA on the Python side, now
    /// exported as a curve for a mastering-meter-style loudness history view
    /// (finer-grained than the Loudness Arc panel's per-section buckets).
    struct TruePeakEvent {
        float time_sec = 0.0f; ///< when the overshoot peaks
        float dbtp = 0.0f;     ///< inter-sample peak magnitude, dBTP
    };
    struct LoudnessCurveResult {
        double duration_sec = 0.0;
        float hop_sec = 1.0f;
        float block_sec = 3.0f;
        std::vector<float> lufs_curve;
        std::vector<float> crest_curve; ///< rolling peak/RMS crest factor (dB), same hop
        // True-peak overshoot distribution (Gemini audit): where inter-sample
        // peaks exceed the ceiling, for the loudness-timeline overlay.
        float true_peak_threshold_dbtp = -1.0f;
        int true_peak_over_count = 0;
        std::vector<TruePeakEvent> true_peak_events;
    };
    khensu::Result<LoudnessCurveResult, BridgeError> analyzeLoudnessCurve(const std::string& file_path);

    /// POST /analyze/sub_bass_phase - kick vs bass phase alignment in the sub/
    /// low band (Gemini audit). Pass the Demucs `drums` stem as the kick - the
    /// analyzer low-passes to isolate the kick's sub energy.
    struct PhaseCorrPoint {
        float time_sec = 0.0f;
        float correlation = 0.0f;
    };
    struct SubBassPhaseResult {
        float cutoff_hz = 120.0f;
        float correlation = 0.0f;   ///< -1..1 whole-track low-band correlation
        float phase_degrees = 0.0f; ///< dominant kick↔bass phase; 0 = in phase
        std::string verdict;
        std::vector<PhaseCorrPoint> correlation_over_time;
        std::vector<std::pair<float, float>> goniometer; ///< (kick, bass) points, -1..1
    };
    khensu::Result<SubBassPhaseResult, BridgeError> analyzeSubBassPhase(const std::string& kick_path,
                                                                        const std::string& bass_path);

    khensu::Result<khensu::ChordAnalysis, BridgeError> analyzeChords(const std::string& file_path);

    /// Generic POST to any /analyze/* (or other) endpoint, returning the raw
    /// JSON response as a juce::var so panels can read fields directly without
    /// a bespoke typed struct per endpoint. `body` is the request object.
    /// Used by the newer analysis panels (streaming normalization, source
    /// forensics, tuning, mono-compat, mid/side + phase + stem + spectral-delta
    /// visualizations).
    /// Heavy synchronous endpoints get a sized timeout from
    /// timeoutForEndpoint() (CORE-03 / WIRE-03) instead of the 30 s default.
    khensu::Result<juce::var, BridgeError> postAnalyzeJson(const std::string& endpoint, const juce::var& body);

    /// The connection/receive timeout postAnalyzeJson() uses for `endpoint`:
    /// a sized ceiling for the known heavy routes (album, comping, take
    /// alignment, pitch correction, word clip gain, MIDI variation, evidence
    /// graph, mix quality, targeted humanize), else 0 = the bridge default.
    static int timeoutForEndpoint(const std::string& endpoint) noexcept;

    /// POST /analyze/vocal - time-resolved vocal metrics. Returned as raw JSON
    /// (sibilance_timeline[], breath_markers[], presence/clarity/warmth_score,
    /// formant_balance{}, window_sec) so panels can read fields directly.
    khensu::Result<juce::var, BridgeError> analyzeVocal(const std::string& file_path);

    /// POST /analyze/source_triage - Fix-at-Source Triage: recording defects
    /// (clipping, dropouts, comb filtering, noise floor, proximity boom, hum,
    /// DC offset) classified fixable/mitigable/rerecord. Raw JSON
    /// ({findings:[{code,title,detail,category,severity}], verdict}) so the
    /// panel reads fields directly.
    /// `material` "mix" for the session's mix or master (CON-7: the intake's
    /// cached result is read back), "auto" to detect it (a recorded take).
    khensu::Result<juce::var, BridgeError> analyzeSourceTriage(const std::string& file_path,
                                                               const juce::String& material = "auto");

    /// POST /analyze/signal_integrity - true-peak (4x oversampled dBTP),
    /// mono-bass compatibility below 120 Hz, and 50/60 Hz hum + hiss noise
    /// floor. Raw JSON ({true_peak{...}, mono_bass{...}, noise_floor{...}},
    /// each with its own `verdict`) so the panel reads fields directly.
    khensu::Result<juce::var, BridgeError> analyzeSignalIntegrity(const std::string& file_path);

    /// POST /analyze/quick_stats - Track Quick Stats aggregate (spec §2.2):
    /// duration, sample rate, bit depth, channels, integrated LUFS, LRA,
    /// true peak dBTP, crest factor, DC offset, stereo width %, clipping
    /// census. Raw JSON; the panel reads fields directly.
    khensu::Result<juce::var, BridgeError> analyzeQuickStats(const std::string& file_path);

    /// POST /analyze/metric_timeline (pipeline #6) - aligned per-window
    /// {loudness_lufs, crest_db, harshness} plus a `hotspots` summary
    /// (loudest / most_squashed / harshest window). Raw JSON.
    /// CE-PLT-2: `baseline_path` (optional) is the audio `file_path` was
    /// rendered from; the service then re-measures only the changed windows.
    khensu::Result<juce::var, BridgeError> analyzeMetricTimeline(const std::string& file_path,
                                                                 const std::string& baseline_path = {});

    /// POST /analyze/measurement_quality (pipeline #5) - per-measurement
    /// {value, confidence, quality_flag, note} trust map over loudness / crest /
    /// true-peak / bpm / key, plus n_reliable / n_total. Raw JSON.
    khensu::Result<juce::var, BridgeError> analyzeMeasurementQuality(const std::string& file_path);

    /// POST /analyze/reference_match (pipeline #8) - nearest professional
    /// reference exemplar(s) with per-metric deltas + guidance. `genre_id`
    /// scopes the search (empty = whole library); `k` = how many. Raw JSON.
    khensu::Result<juce::var, BridgeError> analyzeReferenceMatch(const std::string& file_path,
                                                                 const std::string& genre_id = "", int k = 5,
                                                                 bool include_sections = false);

    /// POST /analyze/mix_quality (pipeline #9) - opt-in self-supervised
    /// mix-defect model. Raw JSON: {available:false,...} until a model is
    /// present + KHENSU_ENABLE_MIX_QUALITY is set, else {available, quality_band,
    /// quality_probability|null, ...}.
    khensu::Result<juce::var, BridgeError> analyzeMixQuality(const std::string& file_path);

    /// POST /analyze/evidence_graph (pipeline #7) - typed causal graph across
    /// findings: {nodes, edges:[{source,target,relation,rationale}], chains}.
    /// `stems` (optional) adds masking-collision nodes. Raw JSON.
    khensu::Result<juce::var, BridgeError> analyzeEvidenceGraph(const std::string& file_path,
                                                                const std::map<std::string, std::string>& stems = {});

    /// Which Analog Feel characters to apply (all on by default).
    struct AnalogFeelCharacter {
        bool tapeDrift = true; ///< wow/flutter: micro-timing + micro-pitch drift
        bool faderRide = true; ///< slow random level ride
        bool roomTone = true;  ///< breathing low-level floor instead of digital silence
    };

    /// POST /humanize/analog_feel - a creative single-pass render of the
    /// chosen characters at `amount` (0..1); with `stems`, tape drift runs
    /// per stem before the sum. No artifact scan and no detector target.
    /// `output_path` must sit beside the source. Raw JSON: {output_path,
    /// amount, applied[], stem_aware, sample_rate_hz}.
    khensu::Result<juce::var, BridgeError> analogFeel(const std::string& file_path, const std::string& output_path,
                                                      float amount, AnalogFeelCharacter character,
                                                      const std::map<std::string, std::string>& stems = {});

    /// POST /analyze/artifacts_scan - AI-artifact detection (spec §5.1):
    /// fused confidence, verdict tier, per-finding evidence, the 9-service
    /// checklist with attribution likelihoods, "Artifacts Summary" text, and
    /// a clipping census with positions in seconds. Raw JSON. `stems` maps
    /// stem names (e.g. "drums", "vocals") to paths; when non-empty the
    /// service's deep tier (per-stem observations) also runs (spec §5.1 heavy).
    /// `includeLearned` asks for the learned watermark/model tiers (AudioSeal,
    /// WavMark - tens of seconds); only an explicit deep scan sets it, never
    /// the Dashboard auto-scan or humanize/compare rescans (AID-08).
    khensu::Result<juce::var, BridgeError> analyzeArtifactsScan(const std::string& file_path,
                                                                const std::map<std::string, std::string>& stems = {},
                                                                bool includeLearned = false);

    /// POST /metadata/clean - copies the file beside the source with only
    /// workflow / personal tags removed (comments, encoder/software, private
    /// app data). Identity, rights and every AI-provenance / disclosure tag
    /// are kept. Raw JSON ({output_path, removed_tag_count, removed_tags,
    /// kept_tags, kept_provenance_tags}).
    khensu::Result<juce::var, BridgeError> cleanDeliveryMetadata(const std::string& file_path);

    /// POST /humanize/deep - Deep Humanize (spec §5.2): humanize each stem
    /// at its own intensity and re-sum to a new mix. `intensities` maps stem
    /// name → 0..1; stems absent from it use `default_intensity`. The four
    /// `*_overrides` maps let individual stems use a different groove shape
    /// (wow rate/depth/level variation/noise floor) than the shared global
    /// scalar - a stem absent from an override map falls back to the global
    /// value, so existing single-groove callers are unaffected. Raw JSON
    /// ({output_path, stems_processed, report:[{stem,intensity,channels}]}).
    /// `output_path` must sit beside the stems; empty lets the service pick a
    /// fixed "deep_humanized.wav" there (overwritten by every run).
    khensu::Result<juce::var, BridgeError>
    deepHumanize(const std::map<std::string, std::string>& stems, const std::map<std::string, float>& intensities,
                 float default_intensity, float wow_rate_hz = 0.3f, float depth_ms = 4.0f, float level_var_db = 0.8f,
                 float noise_floor_db = -72.0f, const std::map<std::string, float>& wow_rate_overrides = {},
                 const std::map<std::string, float>& depth_overrides = {},
                 const std::map<std::string, float>& level_var_overrides = {},
                 const std::map<std::string, float>& noise_floor_overrides = {}, const std::string& output_path = {});

    /// POST /analyze/stem_taxonomy - 9-bucket canonical taxonomy (spec §3):
    /// vocals/bass/synth/kick/snare/percussion/drums-rollup/atmosphere-fx/
    /// other with energy shares, presence, confidence, and a pairwise
    /// collision matrix. Raw JSON ({buckets:[...], collisions:[...],
    /// top_collisions:[...], taxonomy_note}).
    khensu::Result<juce::var, BridgeError>
    analyzeStemTaxonomy(const std::string& vocals_path, const std::string& drums_path, const std::string& bass_path,
                        const std::string& other_path, const std::string& genre_id = "");

    /// POST /analyze/arrangement - five-element arrangement analysis
    /// (foundation/rhythm/pad/lead/fills) over the four separated stems.
    /// Raw JSON ({windows:[{start_sec,end_sec,shares{},present{},silent}],
    /// flags:[{start_sec,end_sec,code,message}], element_presence{}}).
    khensu::Result<juce::var, BridgeError> analyzeArrangement(const std::string& vocals_path,
                                                              const std::string& drums_path,
                                                              const std::string& bass_path,
                                                              const std::string& other_path);

    /// POST /analyze/gain_rider - Smart Gain Rider: phrase-level clip-gain
    /// automation toward a pre-compression RMS target. render=true also
    /// writes <file>_gainride.wav and returns rendered_path. Raw JSON
    /// ({phrases:[{start_sec,end_sec,rms_db,gain_db}], envelope_db[],
    /// window_sec, noise_floor_db, rendered_path}).
    khensu::Result<juce::var, BridgeError> analyzeGainRider(const std::string& file_path, float target_rms_db,
                                                            bool render);

    /// POST /analyze/pitch_accuracy - pYIN-based per-note tuning map: notes
    /// segmented from voiced pitch runs, each with mean cents deviation from
    /// the nearest equal-tempered pitch and a stability (std dev) figure.
    /// Raw JSON ({notes:[{start_sec,end_sec,note_name,note_midi,
    /// mean_cents_deviation,cents_stability}], mean_abs_cents, in_tune_pct,
    /// worst_note, verdict, measurement_reference_hz, detected_reference_hz,
    /// reference_offset_cents, reference_offset_warning}).
    /// `reference_hz` is the measurement grid (the mix's concert A from
    /// analyzeTuningReference); sent only when usableReferenceHz() accepts
    /// it, otherwise the service measures against A=440.
    khensu::Result<juce::var, BridgeError> analyzePitchAccuracy(const std::string& file_path,
                                                                std::optional<double> reference_hz = std::nullopt);

    /// POST /analyze/pitch_curve - note grid for the pitch editor. Raw JSON
    /// ({duration_sec, hop_sec, f0_midi[], notes:[{id,start_sec,end_sec,midi,
    /// target_midi}], reference_hz, detected_reference_hz,
    /// reference_offset_cents, reference_offset_warning}); midi/target_midi
    /// are on the `reference_hz` grid (MIDI 69 == reference_hz).
    khensu::Result<juce::var, BridgeError> analyzePitchCurve(const std::string& file_path,
                                                             std::optional<double> reference_hz = std::nullopt);

    /// POST /analyze/tuning_reference - the track's concert-A estimate. Raw
    /// JSON ({reference_hz, cents_from_440, verdict, drift_range_cents,
    /// drifting, drift[]}).
    khensu::Result<juce::var, BridgeError> analyzeTuningReference(const std::string& file_path);

    /// POST /analyze/sound_recreation - timbre class + extracted MIDI summary
    /// + plugin recipes. `reference_hz` puts the pitched extraction on the
    /// mix's tuning grid (sent only when usable).
    khensu::Result<juce::var, BridgeError> analyzeSoundRecreation(const std::string& file_path,
                                                                  const std::string& stem_type,
                                                                  std::optional<double> reference_hz = std::nullopt);
    /// The service's accepted reference window (Pydantic ge/le on
    /// reference_hz) - anything outside it is a 422.
    static constexpr double kMinReferenceHz = 400.0;
    static constexpr double kMaxReferenceHz = 480.0;
    /// `reference_hz` if it is finite and inside [kMinReferenceHz,
    /// kMaxReferenceHz], else nullopt (=> the service's A=440 default).
    static std::optional<double> usableReferenceHz(std::optional<double> reference_hz) noexcept;

    /// POST /analyze/depth - Depth & Dimension Mapper: per-stem front-back
    /// placement from wetness/darkness/softness cues. Raw JSON
    /// ({stems:[{stem,depth,wetness,darkness,softness}], suggestions[]}).
    khensu::Result<juce::var, BridgeError> analyzeDepth(const std::map<std::string, std::string>& stems);

    /// POST /analyze/gain_xray - Compression X-Ray: effective dynamic gain
    /// curve of processed vs original, with pumping / over-compression
    /// events. Raw JSON ({gain_curve_db[], window_sec, events[], verdict,
    /// avg_gain_db, max_reduction_db, static_offset_db, pumping_score}).
    khensu::Result<juce::var, BridgeError> analyzeGainXray(const std::string& original_path,
                                                           const std::string& processed_path);

    /// POST /analyze/album - Album Assembly: cross-track loudness/tilt/crest
    /// consistency for an ordered tracklist. Raw JSON ({tracks[], flags[],
    /// lufs_spread, verdict}).
    khensu::Result<juce::var, BridgeError> analyzeAlbum(const std::vector<std::string>& track_paths);

    /// POST /analyze/pan_balance - per-band L/R energy bias over time +
    /// per-band width and warnings. Raw JSON ({bands[], bias_curves{},
    /// band_summary[], warnings[], window_sec}).
    khensu::Result<juce::var, BridgeError> analyzePanBalance(const std::string& file_path);

    /// POST /analyze/comping - Comping Assistant: per-phrase take scoring
    /// (pitch/level/tone) + best-take proposal; render=true also writes the
    /// crossfaded comp. Raw JSON ({phrases:[{start_sec,end_sec,takes[],
    /// best_take}], take_count, rendered_path}).
    khensu::Result<juce::var, BridgeError> analyzeComping(const std::vector<std::string>& take_paths, bool render);

    /// POST /analyze/lyrics - word-level transcription (faster-whisper) +
    /// per-word vocal-presence-band masking check. Slow (transcription) and
    /// needs the optional [asr] Python dependency - 503 (surfaced as
    /// BridgeError::Kind::HttpError with status 503) if it isn't installed.
    /// Long timeout (5 min) to cover first-run model download + transcription
    /// of a full track; call from a background thread, never the UI thread.
    khensu::Result<khensu::AnalysisResult::LyricIntelligibilityData, BridgeError>
    analyzeLyrics(const std::string& file_path, const std::string& model_size = "base");
};

} // namespace khensu::ipc
