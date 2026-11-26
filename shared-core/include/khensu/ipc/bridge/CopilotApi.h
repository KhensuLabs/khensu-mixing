#pragma once
#include <khensu/ipc/BridgeTransport.h>

namespace khensu::ipc {

/// Copilot: chat, chains, renders, mastering agent, models, transport, feedback (ENG-4). Implemented in
/// src/ipc/HttpServiceBridge_Copilot.cpp. Usable on its own (`CopilotApi{}.method(...)`)
/// or through HttpServiceBridge, which derives from every domain client.
class CopilotApi : public virtual BridgeTransport {
  public:
    CopilotApi() = default;
    explicit CopilotApi(std::string base_url, int timeout_ms = kDefaultTimeoutMs,
                        int retry_delay_ms = kDefaultRetryDelayMs)
        : BridgeTransport(std::move(base_url), timeout_ms, retry_delay_ms)
    {}

    /// POST /copilot/decode_feedback - Client Revision Decoder: maps client
    /// feedback language to ranked candidate parameter changes. Raw JSON
    /// ({matches:[{concept,matched_text,interpretations:[{reading,
    /// explanation,parameters{},confidence}]}], unmatched}).
    khensu::Result<juce::var, BridgeError> decodeClientFeedback(const std::string& feedback_text);

    /// POST /copilot/render_device_preview - Translation Check: renders the
    /// mix as a playback device presents it ("phone"/"earbuds"/"car"/"club").
    /// Raw JSON ({device, rendered_path, note}).
    khensu::Result<juce::var, BridgeError> renderDevicePreview(const std::string& file_path, const std::string& device);

    /// POST /report/storyboard - client-facing Revision Storyboard: writes a
    /// standalone HTML chronology of the session's client-visible changes.
    /// Raw JSON ({saved_path, event_count}).
    khensu::Result<juce::var, BridgeError> generateStoryboard(const std::string& session_id,
                                                              const std::string& output_path, const std::string& title);

    // ---- Copilot / Chat Extensions ----
    struct EffectSpec {
        juce::String effect;
        std::map<juce::String, float> parameters;
    };

    struct CopilotChatResponse {
        juce::String response;
        std::vector<EffectSpec> suggested_chain;
        juce::String target_stem;
        juce::String source;
        juce::String model_name;
        double response_time_sec = 0.0;
        juce::String provenance_reason;
        // Fingerprint of (source, model, prompt, inputs) that produced this
        // response - lets the UI show a "why" trace and tie a later render
        // back to exactly what decided it. See llm_config.decision_hash.
        juce::String decision_hash;
        /// streamChat only: the server's SSE {"type":"error"} message when the
        /// model stream broke after partial output (CORE-14). `response` then
        /// holds only what was streamed before the failure - show both.
        juce::String stream_error;
    };

    // ---- Mixer Chain Generator Extensions ----
    struct MixerChainPlugin {
        juce::String name;
        juce::String category;
        juce::String settings_description;
        juce::String ecosystem;
    };

    /// Populated only when KHENSU_LLM_CONSENSUS_PROVIDERS queried 2+ models for
    /// this decision (mixer_chain_generator._consensus_chain). agreement is
    /// "single" | "unanimous" | "high" | "low"; agreement_score is empty for
    /// "single" (no pairwise comparison to make).
    struct ConsensusInfo {
        juce::String agreement;
        std::optional<double> agreement_score;
        juce::StringArray providers_queried;
        int providers_responded = 0;
        juce::StringArray alternative_descriptions;
    };

    struct MixerChainResponse {
        juce::String description;
        std::vector<MixerChainPlugin> plugins;
        // See CopilotChatResponse::decision_hash.
        juce::String decision_hash;
        std::optional<ConsensusInfo> consensus;
    };

    khensu::Result<MixerChainResponse, BridgeError>
    generateMixerChain(const juce::String& targetChannel, const juce::String& genre, const juce::String& energy,
                       const juce::String& mood, double bpm, const juce::String& styleDescription,
                       const juce::StringArray& enabledEcosystems, const juce::String& vocalStrategy = {},
                       const juce::var& vocalMacros = {}, double preferenceLufsOffset = 0.0,
                       double preferenceLraOffset = 0.0, double preferenceStereoWidthOffset = 0.0,
                       double preferenceLowEndWeightOffset = 0.0);

    // ---- Copilot chat job queue ----
    // Chat over the job queue /copilot/chat/start|status|cancel.
    // A chat turn blocks on a full LLM round trip; this lets the console show
    // a live "thinking..." state and cancel instead of freezing the UI thread.
    struct ChatJobStatus {
        juce::String status; // "pending" | "running" | "completed" | "failed" | "cancelled"
        juce::String error;
        std::optional<CopilotChatResponse> result; // set only when status == "completed"
    };

    khensu::Result<juce::String, BridgeError>
    startChatJob(const juce::String& message, const juce::var& history, const juce::var& analysisResult,
                 const juce::String& genreContext, double bpm, const std::vector<EffectSpec>& currentChain = {},
                 const juce::String& currentTargetStem = {}, const juce::String& personality = "engineer",
                 const juce::String& verbosity = "engineer");

    khensu::Result<ChatJobStatus, BridgeError> getChatJobStatus(const juce::String& job_id);
    void cancelChatJob(const juce::String& job_id);

    /// Streams a chat reply token-by-token via /copilot/chat/stream (SSE,
    /// added server-side in commit 513499f but left without a C++ consumer).
    /// Blocking - call from a background thread, not the message thread.
    /// onToken fires once per partial text chunk as it arrives; onDone fires
    /// once with the complete structured response (decision_hash, suggested
    /// chain, etc.) when the stream's final event lands; onError fires
    /// instead if the request can't even be started. shouldCancel is polled
    /// between reads so a caller can abort a long-running stream early.
    void streamChat(const juce::String& message, const juce::var& history, const juce::var& analysisResult,
                    const juce::String& genreContext, double bpm, const std::vector<EffectSpec>& currentChain,
                    const juce::String& currentTargetStem, const juce::String& personality,
                    const juce::String& verbosity, std::function<void(const juce::String& tokenText)> onToken,
                    std::function<void(const CopilotChatResponse&)> onDone,
                    std::function<void(const BridgeError&)> onError, std::function<bool()> shouldCancel = {});

    // ---- Mixer Chain job queue ----
    // The synchronous generateMixerChain() above blocks on a full LLM round
    // trip with no progress feedback. /copilot/generate_mixer_chain/start
    // (added in commit cef8506) already exists server-side but had no C++
    // client or UI consumer - this closes that gap so the Copilot tab can
    // show "Generating..." with a Cancel button instead of a frozen button.
    struct MixerChainJobStatus {
        juce::String status; // "pending" | "running" | "completed" | "failed" | "cancelled" | "interrupted"
        juce::String error;
        std::optional<MixerChainResponse> result; // set only when status == "completed"
    };

    khensu::Result<juce::String, BridgeError>
    startMixerChainJob(const juce::String& targetChannel, const juce::String& genre, const juce::String& energy,
                       const juce::String& mood, double bpm, const juce::String& styleDescription,
                       const juce::StringArray& enabledEcosystems, const juce::String& vocalStrategy = {},
                       const juce::var& vocalMacros = {}, double preferenceLufsOffset = 0.0,
                       double preferenceLraOffset = 0.0, double preferenceStereoWidthOffset = 0.0,
                       double preferenceLowEndWeightOffset = 0.0);

    khensu::Result<MixerChainJobStatus, BridgeError> getMixerChainJobStatus(const juce::String& job_id);

    void cancelMixerChainJob(const juce::String& job_id);

    // ---- Plugin-aware chain resolution ----
    // matcher.py resolves generic effect names ("Compressor", "DeEsser"...)
    // against the user's scanned_plugins table in khensu.db. Both endpoints
    // existed server-side with no C++ consumer until now.
    struct MatchedPlugin {
        juce::String name, manufacturer, category, format, file_path, id;
    };
    struct ResolvedEffect {
        juce::String effect;                         // the generic name that was asked for
        std::optional<MatchedPlugin> matched_plugin; // empty = no local plugin matched
    };

    /// POST /copilot/resolve_chain - resolve generic effect names to the
    /// user's scanned local plugins. Response order matches request order.
    khensu::Result<std::vector<ResolvedEffect>, BridgeError> resolveChain(const juce::StringArray& genericEffects);

    /// GET /analyze/metadata - service/library version diagnostics as raw
    /// JSON (service_version, librosa/numpy versions, analysis schema and
    /// genre-heuristic versions). Shown on the System Brain screen.
    khensu::Result<juce::var, BridgeError> getServiceMetadata();

    // Returns the cached staged intake bundle, preserving stage results and
    // musical-context evidence for UI consumers.
    juce::var fetchFullIntake(const juce::String& file_path, const juce::String& stated_genre_id = {},
                              const juce::String& reference_path = {}, const juce::String& selection_mode = "detected",
                              bool use_genre_targets = true, bool confirm_detected_genre = false);
    // The retry rebuilds the bundle under the same cache key as the live
    // context, so it takes the same intake settings startFullIntake() does.
    juce::var retryFullIntakeStage(const juce::String& file_path, const juce::String& stage,
                                   const juce::String& stated_genre_id = {}, const juce::String& reference_path = {},
                                   const juce::String& selection_mode = "detected", bool use_genre_targets = true,
                                   bool confirm_detected_genre = false);
    juce::var startFullIntake(const juce::String& file_path, const juce::String& stated_genre_id = {},
                              const juce::String& reference_path = {}, const juce::String& selection_mode = "detected",
                              bool use_genre_targets = true, bool confirm_detected_genre = false);
    juce::var fullIntakeStatus(const juce::String& job_id);
    juce::var cancelFullIntake(const juce::String& job_id);

    // ---- Transport / real-time sync extensions ----
    // The plugin POSTs this state from the DAW; the desktop app GETs it to
    // follow the playhead and drive real-time meters. Every Copilot instance
    // posts under its own `instance_id` (PLG-04 / SVC-16), so several
    // instances in one DAW no longer overwrite one global slot.
    struct PlayheadState {
        float position_sec = 0.0f;
        bool is_playing = false;
        float left_peak_db = -96.0f;
        float right_peak_db = -96.0f;
        float momentary_lufs = -96.0f;
        float short_term_lufs = -96.0f;
        float correlation = 0.0f;
        float width = 1.0f;
        /// POST: the posting plugin instance (empty = legacy, untargeted).
        /// GET: the instance whose state this is.
        juce::String instance_id;
        /// GET only: every instance the service currently knows about.
        std::vector<juce::String> instances;
        /// GET only: a plugin posted within the service's staleness window.
        /// When false the meter fields are service defaults, not readings.
        bool connected = false;
    };

    /// GET /transport/playhead - polled by the desktop app. An empty
    /// `instanceId` returns the most recently posting instance.
    khensu::Result<PlayheadState, BridgeError> getPlayhead(const juce::String& instanceId = {});

    /// POST /transport/playhead - called by the plugin. Synchronous; run on a
    /// background thread. Returns false on any transport/HTTP failure.
    bool postPlayhead(const PlayheadState& state);

    /// GET /transport/seek?since=<generation> - polled by every plugin
    /// instance. The service no longer consumes the seek on the first poll:
    /// each instance passes the last generation it applied, and `pending` is
    /// true while a newer seek exists, so EVERY instance sees each seek.
    /// `generation` is -1 when the service predates generations (legacy
    /// consume-on-read semantics; `pending` alone is meaningful then).
    struct SeekState {
        float position_sec = 0.0f;
        bool pending = false;
        int generation = -1;
    };
    khensu::Result<SeekState, BridgeError> pollSeek(int sinceGeneration = -1);

    /// POST /transport/params - desktop pushes plugin DSP parameter targets.
    /// An empty `targetInstanceId` addresses every instance; otherwise only
    /// that instance applies them.
    bool postPluginParams(const std::map<juce::String, float>& params, const juce::String& targetInstanceId = {});

    /// GET /transport/params?instance_id=X - polled by the plugin. The service
    /// returns params only when the push was untargeted or targeted at X.
    /// `generation` increments on each push so the plugin applies a set only
    /// once.
    struct PluginParams {
        int generation = 0;
        std::map<juce::String, float> params;
    };
    khensu::Result<PluginParams, BridgeError> pollPluginParams(const juce::String& instanceId = {});

    // ---- Copilot explain extension ----
    /// POST /copilot/explain - plain-language explanation of a recommendation,
    /// plus the service's deterministic narrative (text_factory) and myth
    /// context (myth_guardrail), whichever source wrote the explanation.
    struct ExplainResult {
        juce::String explanation;
        juce::String source; ///< LLM provider name, "local_llm" or "template"
        bool isMythAdjacent = false;
        juce::String mythClarification;
        juce::String mostLikelyCause, tradeOff, howToVerify;
    };
    khensu::Result<ExplainResult, BridgeError>
    explainRecommendation(const juce::String& issueCode, const juce::String& what, const juce::String& suggestedAction,
                          float severity01, const juce::String& whyHelps, const juce::String& engineerNote,
                          const juce::String& genreContext, double bpm);

    // ---- Feedback mirror extension ----
    /// POST /feedback - mirrors recommendation acted-on state into the Python
    /// service store so reports/analytics see the same history as the C++
    /// FeedbackManager. Returns false on failure (callers treat as best-effort).
    /// `httpStatusOut` (optional) receives the HTTP status, 0 when no response
    /// arrived - the outbox uses it to dead-letter permanent 4xx rejections.
    bool postFeedback(const juce::String& recommendationId, bool actedOn, const juce::String& note,
                      int* httpStatusOut = nullptr);

    // ---- Provenance event layer ----
    struct ProvenanceEvent {
        juce::String id, session_id, event_type, summary, recommendation_id, created_at;
        juce::var payload;
    };

    /// POST /events - append a provenance event (best-effort; returns false on failure).
    bool postEvent(const juce::String& sessionId, const juce::String& eventType, const juce::String& summary,
                   const juce::String& recommendationId = {}, const juce::var& payload = {},
                   int* httpStatusOut = nullptr);

    /// GET /events?session_id=... - timeline for a session, newest first.
    khensu::Result<std::vector<ProvenanceEvent>, BridgeError> getEvents(const juce::String& sessionId, int limit = 200);

    // ---- Data erasure (audit DATA-02) ----
    /// DELETE /events/session/{sessionId} - erases this session's provenance
    /// events. Best-effort (returns false on failure); the local khensu.db
    /// deletion is the authoritative half and should proceed regardless.
    bool deleteEventsForSession(const juce::String& sessionId);

    /// DELETE /events - erases every provenance event. Best-effort.
    bool deleteAllEvents();

    /// DELETE /feedback - erases every feedback row. Best-effort.
    bool deleteAllFeedback();

    // ---- Render job queue ----
    // Renders over the job queue /copilot/apply_chain/start (the synchronous
    // /copilot/apply_chain and /apply_graph wrappers were removed - unused).
    // The server's ApplyChainRequest carries either a flat effects list or a
    // {branches, post} graph, so one queue serves both render styles. Added
    // server-side in commit cef8506; this is its first C++ consumer.
    struct RenderJobStatus {
        juce::String status; // "pending" | "running" | "completed" | "failed" | "cancelled"
        juce::String error;
        juce::String outputPath; // set when status == "completed"
        juce::var result;        // full payload: applied_effects, skipped_effects, harmonic_reports
    };

    /// `targetStem` (audit AI-02): forwarded as ApplyChainRequest.target_stem
    /// so the server's stem-attribution guardrail validates it against
    /// filePath/stemPaths on every entry path, not just the synchronous
    /// /copilot/apply_chain endpoint. Empty defaults to "mix" server-side.
    /// `decisionHash`: the CopilotChatResponse/MixerChainResponse::decision_hash
    /// of the model decision that proposed the chain, so the render's
    /// provenance event traces back to it - omitted from the body when empty.
    /// `keyStem` (ENH-6): ApplyChainRequest.key_stem, the stem in `stemPaths`
    /// that keys the chain's DynamicEQ stages with use_key 1 - omitted when
    /// empty (the service then keys them on their own input).
    khensu::Result<juce::String, BridgeError>
    startApplyChainJob(const juce::String& filePath, const juce::String& outputPath,
                       const std::vector<EffectSpec>& chain, const juce::String& sessionId = {},
                       const juce::String& sourceRecommendationId = {},
                       const std::map<juce::String, juce::String>& stemPaths = {}, bool genreRelative = false,
                       bool genreActionAllowed = false, const juce::String& abstainedReason = {},
                       const juce::String& targetStem = {}, const juce::String& decisionHash = {},
                       const juce::String& keyStem = {});

    khensu::Result<juce::String, BridgeError>
    startApplyGraphJob(const juce::String& filePath, const juce::String& outputPath, const juce::var& branches,
                       const juce::var& post, bool normalize, const juce::String& sessionId = {},
                       const juce::String& sourceRecommendationId = {},
                       const std::map<juce::String, juce::String>& stemPaths = {}, bool genreRelative = false,
                       bool genreActionAllowed = false, const juce::String& abstainedReason = {});

    khensu::Result<RenderJobStatus, BridgeError> getApplyChainJobStatus(const juce::String& job_id);
    /// GET /copilot/apply_chain/events/{id} (PERF-6): follows the render as
    /// server-sent events, calling onStatus for each change (on this thread),
    /// until a terminal status (returns true) or the stream ends, fails or
    /// shouldCancel() turns true (returns false - the caller polls instead).
    bool followApplyChainJob(const juce::String& job_id, const std::function<void(const RenderJobStatus&)>& onStatus,
                             std::function<bool()> shouldCancel = {});

    void cancelApplyChainJob(const juce::String& job_id);

    // ---- Batch render job queue ----
    // One chain applied to every audio file in a folder over
    // /copilot/apply_chain/batch/start - the processing counterpart of the
    // album analysis pipeline. Per-file success/failure lives in `results`;
    // a corrupt file doesn't fail the batch.
    struct BatchRenderJobStatus {
        juce::String status; // "pending" | "running" | "completed" | "failed" | "cancelled"
        int total = 0;       // files in the batch
        int done = 0;        // files processed so far (success or failure)
        int failed = 0;      // subset of done that errored
        juce::var results;   // array of {file_path, output_path, status, applied_effects, error}
    };

    /// Returns the job id. outputDir empty -> service default (<folder>/khensu_batch).
    /// `decisionHash`: the CopilotChatResponse/MixerChainResponse::decision_hash
    /// of the model decision that proposed the chain, so the render's
    /// provenance event traces back to it - omitted from the body when empty.
    khensu::Result<juce::String, BridgeError>
    startApplyChainBatchJob(const juce::String& inputDir, const juce::String& outputDir,
                            const std::vector<EffectSpec>& chain, const juce::String& suffix = "_processed",
                            const juce::String& sessionId = {}, const juce::String& decisionHash = {});

    khensu::Result<BatchRenderJobStatus, BridgeError> getApplyChainBatchJobStatus(const juce::String& job_id);

    void cancelApplyChainBatchJob(const juce::String& job_id);

    // ---- One-click platform-target master render set ----
    // Renders the master to each platform's target loudness (Spotify -14 /
    // Apple -16 / club -9 / YouTube), true-peak safe and through the post-render
    // guardrail, in one action (khensu_services.copilot.platform_render).
    struct PlatformMastersJobStatus {
        juce::String status; // "pending" | "running" | "completed" | "failed" | "cancelled"
        int total = 0;
        int done = 0;
        juce::String current; // platform id currently rendering (empty when idle/done)
        juce::var results;    // array of {platform_id, platform_name, status,
                              //           output_path, achieved_lufs,
                              //           achieved_true_peak_dbtp, target_lufs, guardrail}
    };

    /// Returns the job id. platforms empty -> the full default set; outputDir
    /// empty -> service default (<source dir>/platform_masters). Each version
    /// keeps the master's loudness and is made true-peak safe for its platform
    /// (CUT-9 safety variants) unless matchLoudness asks for the platform's
    /// loudness target too.
    khensu::Result<juce::String, BridgeError> startPlatformMastersJob(const juce::String& filePath,
                                                                      const juce::String& outputDir = {},
                                                                      const juce::StringArray& platforms = {},
                                                                      bool matchLoudness = false);

    khensu::Result<PlatformMastersJobStatus, BridgeError> getPlatformMastersJobStatus(const juce::String& job_id);

    void cancelPlatformMastersJob(const juce::String& job_id);

    // ---- AI mastering agent ----
    // render -> analyze -> critique -> adjust, iterated toward a genre's
    // numeric targets. Job-queue only (a multi-iteration render loop can take
    // a while); see khensu_services.copilot.mastering_agent for the loop.
    struct MasteringIteration {
        int iteration = 0;
        juce::String critique; // human-readable "what changed and why"
        juce::String outputPath;
        bool converged = false;
        std::vector<EffectSpec> chain; ///< the chain this pass rendered (numeric parameters)
        juce::var metrics; // {lufs, lra, crest_db, centroid_hz, ...}
        juce::var deltas;  // {lufs: {measured, target, delta}, ...}
        // Vocal-band/artifact terms folded into the objective score (not just
        // the pass/fail guardrail gate) - std::nullopt on iteration 1, which
        // has no prior render to compare against. See mastering_agent.py's
        // _safety_objective_terms().
        std::optional<double> vocalPresenceDelta;
        std::optional<double> artifactScoreDelta;
    };
    struct MasteringAgentResult {
        bool converged = false;
        bool requiresUserChoice = true;
        juce::String finalOutputPath;
        juce::String decisionHash;
        juce::var guardrail; // matched-level post-render safety result
        std::vector<MasteringIteration> iterations;
        // ENH-5: the style the run used and the targets it converged toward.
        juce::String style;        // warm | balanced | open | loud
        double strength = 0.5;     // 0..1
        juce::String styleSummary; // one line: what the style moved
        juce::var targets;         // {lufs, crest_db, centroid_hz, ceiling_dbtp}
    };
    struct MasteringJobStatus {
        juce::String status; // "pending" | "running" | "completed" | "failed" | "cancelled"
        juce::String error;
        std::optional<MasteringAgentResult> result;
    };

    khensu::Result<juce::String, BridgeError>
    startMasteringAgentJob(const juce::String& filePath, const juce::String& genreId,
                           const std::vector<EffectSpec>& initialChain, int maxIterations = 5,
                           bool genreActionAllowed = false, const juce::String& abstainedReason = {},
                           const std::map<juce::String, juce::String>& stemPaths = {},
                           const juce::String& style = "balanced", double strength = 0.5);
    khensu::Result<MasteringJobStatus, BridgeError> getMasteringAgentJobStatus(const juce::String& job_id);
    void cancelMasteringAgentJob(const juce::String& job_id);

    // ---- ENH-4: per-file smart batch ----
    // POST /copilot/batch/smart/start | GET status | POST cancel: every file
    // analysed, fixed (the verified agent loop) and optionally mastered (the
    // mastering agent in a style) on its own terms; results per file.
    struct SmartBatchRequest {
        std::vector<juce::String> filePaths;
        juce::String outputDir; // "" = <first file's folder>/khensu_smart_batch
        juce::String suffix{"_smart"};
        bool fix = true;
        bool master = false;
        juce::String genreId;
        bool genreActionAllowed = false;
        juce::String abstainedReason;
        juce::String style{"balanced"};
        double strength = 0.5;
        double targetLufs = -14.0;
        double loudnessOffsetDb = 0.0;      // a macro set's Loudness: shifts the mastering target
        std::optional<double> ceilingDbtp;  // a macro set's Ceiling; empty = the genre's own
    };
    struct SmartBatchFileResult {
        juce::String filePath, outputPath, status, error, fixSummary, masterSummary;
        std::optional<double> beforeLufs, beforeTruePeak, afterLufs, afterTruePeak;
        int findingsBefore = 0, findingsAfter = 0, findingsCleared = 0, keptSteps = 0;
        bool mastered = false;
        juce::StringArray cleared, remaining;
    };
    struct SmartBatchStatus {
        juce::String status, error, outputDir, current, stage;
        int total = 0, done = 0, failed = 0;
        std::vector<SmartBatchFileResult> results;
    };
    struct SmartBatchStart {
        juce::String jobId, outputDir;
        int totalFiles = 0;
    };
    khensu::Result<SmartBatchStart, BridgeError> startSmartBatch(const SmartBatchRequest& request);
    khensu::Result<SmartBatchStatus, BridgeError> getSmartBatchStatus(const juce::String& jobId);
    void cancelSmartBatch(const juce::String& jobId);

    // ---- Delta solo (null test) ----
    /// POST /copilot/render_delta - renders (wet - dry) so the desktop app
    /// can audition only what a processing chain changed. Fast (one
    /// subtract), so it's a plain synchronous call, no job queue.
    khensu::Result<juce::String, BridgeError> renderDeltaSolo(const juce::String& dryPath, const juce::String& wetPath,
                                                              const juce::String& outputPath,
                                                              float normalizeGainDb = 0.0f);

    // ---- Section-aware processing ----
    // A different (or absent) effect chain per detected song section, so a
    // chorus/drop can carry more intensity than a verse instead of one chain
    // for the whole track. Job-queue only (a per-section stitched render can
    // take a while); see khensu_services.copilot.dsp.apply_section_chains.
    struct SectionChainSpec {
        juce::String label;
        double startSec = 0.0, endSec = 0.0;
        std::vector<EffectSpec> effects; // empty = pass this section through dry
    };
    struct SectionChainReport {
        juce::String label;
        std::vector<juce::String> applied, skipped;
    };
    struct SectionChainJobStatus {
        juce::String status; // "pending" | "running" | "completed" | "failed" | "cancelled"
        juce::String error;
        juce::String outputPath;                        // set when completed
        juce::var guardrail;                            // matched-level post-render safety result
        std::vector<SectionChainReport> sectionReports; // set when completed
    };

    khensu::Result<juce::String, BridgeError> startSectionChainJob(
        const juce::String& filePath, const juce::String& outputPath, const std::vector<SectionChainSpec>& sections,
        double crossfadeMs = 30.0, const juce::String& sessionId = {}, const juce::String& sourceRecommendationId = {},
        bool genreRelative = false, bool genreActionAllowed = false, const juce::String& abstainedReason = {},
        const std::map<juce::String, juce::String>& stemPaths = {}, const juce::String& decisionHash = {});
    khensu::Result<SectionChainJobStatus, BridgeError> getSectionChainJobStatus(const juce::String& job_id);
    void cancelSectionChainJob(const juce::String& job_id);

    // ---- Model Downloader Extensions ----
    struct ModelStatus {
        juce::String status = "idle";
        bool installed = false;
        juce::int64 downloaded_bytes = 0;
        juce::int64 total_bytes = 0;
        float progress = 0.0f;
        float speed_kbps = 0.0f;
        float eta_seconds = 0.0f;
        juce::String error_message;
        juce::String models_dir;
        // False when torch/torchaudio/demucs aren't installed - the checkpoint
        // file can be fully "installed" while separation still silently falls
        // back to HPSS because these optional packages are missing.
        bool demucs_package_available = false;
    };

    khensu::Result<ModelStatus, BridgeError> getModelStatus();
    // The mutations return the BridgeError (not a bool) so a caller can tell
    // "downloads are disabled in Settings" (403 + FastAPI detail) or a rejected
    // folder (422) apart from an unreachable service (ui/shared/ServiceErrorHint.h).
    khensu::Result<void, BridgeError> setModelsDir(const juce::String& path);
    khensu::Result<void, BridgeError> startModelDownload();
    khensu::Result<void, BridgeError> pauseModelDownload();
    khensu::Result<void, BridgeError> stopModelDownload();

    // ---- Watermark model pre-cache ----
    // AudioSeal + WavMark detectors download from HF Hub on first load, which
    // would hang the analysis thread; pre-caching fetches the weights ahead of
    // time so enabling detection never blocks a scan (service-side detail in
    // khensu_services/analysis/artifacts/watermark_precache.py).
    struct WatermarkModelStatus {
        juce::String status = "idle"; // idle | downloading | completed | failed
        juce::String current_model;   // which model is downloading right now
        int done = 0;                 // models finished this run
        int total = 0;                // models to fetch
        juce::String error_message;
        bool audioseal_available = false; // package importable
        bool wavmark_available = false;
        bool audioseal_cached = false; // weights pre-cached (gate auto-opens)
        bool wavmark_cached = false;
    };
    khensu::Result<WatermarkModelStatus, BridgeError> getWatermarkModelStatus();
    /// 403 (with the detail) when model downloads / HF Hub are off in Settings.
    khensu::Result<void, BridgeError> startWatermarkModelPrecache();
    /// POST /analysis/cache/clear (FIX-15): deletes every cached analysis
    /// result on disk; returns how many were removed.
    khensu::Result<int, BridgeError> clearAnalysisCache();
    /// POST /genre/examples (GEN-1e): remember the genre the user confirmed
    /// ("confirmed") or chose ("chosen") for a track, so the genre suggestion
    /// learns their music. Returns whether the service stored it (it cannot
    /// without the genre probe).
    khensu::Result<bool, BridgeError> addGenreExample(const juce::String& filePath, const juce::String& genreId,
                                                      const juce::String& source);
    /// DELETE /genre/examples: forget every example; returns how many.
    khensu::Result<int, BridgeError> clearGenreExamples();
    /// GET /genre/examples (AUTO-4): {examples, genres: {genre id: count}}
    /// (api::fields::GenreExamplesSummaryResponse).
    khensu::Result<juce::var, BridgeError> genreExamplesSummary();

    // ---- Learned-model manifest ----
    // One entry per bundled *.onnx model with provenance + integrity, backing
    // the desktop Settings "Offline models" display (service-side detail in
    // khensu_services/analysis/model_manifest.py).
    struct ModelManifestEntry {
        juce::String name;
        juce::String filename;
        juce::int64 size_bytes = 0;
        juce::String sha256;
        juce::String downloaded_at; // ISO-8601
        juce::String provenance;    // may be empty
        juce::String version;       // may be empty
        juce::String source_url;    // may be empty
        bool installed = false;
        bool available = false;
        juce::String unavailable_reason; // stable, redacted code when unavailable
        juce::String remediation_hint;   // safe user-facing next step
        bool intervention_validated = false;
    };
    struct ModelManifest {
        juce::String models_dir;
        bool any_unvalidated = false;
        int available_count = 0;
        int unavailable_count = 0;
        std::vector<ModelManifestEntry> models;
    };
    khensu::Result<ModelManifest, BridgeError> getModelManifest();

    // Per-model update check against each sidecar's source_url. status is one of
    // up_to_date / update_available / no_source / offline / unreachable.
    struct ModelUpdateStatus {
        juce::String name;
        juce::String status;
        juce::String local_version;  // may be empty
        juce::String remote_version; // may be empty
    };
    struct ModelUpdateReport {
        bool online = false;
        bool any_update_available = false;
        std::vector<ModelUpdateStatus> models;
    };
    khensu::Result<ModelUpdateReport, BridgeError> checkModelUpdates();

    // ---- Delivery & QA toolset ----
    // Thin wrappers over the mastering/delivery service endpoints. Each returns
    // the parsed JSON response as a juce::var (the panels read the fields they
    // need) rather than a bespoke result struct per feature.
    khensu::Result<juce::var, BridgeError> auditCodecs(const juce::String& filePath,
                                                       const juce::StringArray& codecs = {});
    khensu::Result<juce::var, BridgeError> ditherPreview(const juce::String& filePath, int targetBits = 16);
    /// POST /copilot/reference_chain; `amount` 0..1 is how far toward the
    /// reference (FEAT-3). The response carries predicted_curve.
    khensu::Result<juce::var, BridgeError> referenceChain(const juce::String& filePath,
                                                          const juce::String& referencePath, double amount = 1.0);
    khensu::Result<juce::var, BridgeError> complianceSheet(const juce::String& filePath,
                                                           const juce::String& pdfPath = {});
    khensu::Result<juce::var, BridgeError> crossfeedAb(const juce::String& filePath);
    khensu::Result<juce::var, BridgeError> abCritique(const juce::String& pathA, const juce::String& pathB);
    khensu::Result<juce::var, BridgeError> albumNormalization(const juce::StringArray& trackPaths,
                                                              const juce::String& platform = "spotify");
};

} // namespace khensu::ipc
