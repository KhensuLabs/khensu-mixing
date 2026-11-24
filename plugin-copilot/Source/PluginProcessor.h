#pragma once
#include <JuceHeader.h>
#include <khensu/common/ThreadContext.h>
#include <khensu/analysis/AnalysisResult.h>
#include <khensu/recommendations/Recommendation.h>
#include <khensu/ipc/ServiceHealthTracker.h>
#include "HostedPluginChainManager.h"
#include "CopilotAnalysisController.h"
#include "CopilotTransportController.h"
#include "CopilotOutputStage.h"
#include "CopilotCorrectiveDsp.h"
#include "CaptureRecorder.h"
#include "CopilotRealtimeTelemetry.h"
#include "HostedPluginEditorWindow.h"
#include "mixhub/MixHub.h"
#include "timeline/IssueTimeline.h"
#include "live/CopilotLiveAnalyzer.h"
#include "automation/FixAutomation.h"
#include "ara/AraSession.h"
#include "CopilotPresets.h"
#include "CopilotUndoHistory.h"
#include <khensu/mixerchain/RecommendationDspMapper.h>
#include <atomic>
#include <array>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace khensu {

// THREAD_CONTEXT: processBlock → AudioThread
//                 createEditor, getStateInformation, loadLastSessionAsync → UIThread
//                 prepareToPlay → UIThread (called before playback starts)
//                 CopilotTransportController → owned worker, joined in the destructor
//                 timerCallback (retirement reaper, audit ARCH-04) → UIThread
class KhensuCopilotProcessor final : public juce::AudioProcessor,
#if JucePlugin_Enable_ARA
                                     public juce::AudioProcessorARAExtension,
#endif
                                     private juce::Timer,
                                     private juce::AsyncUpdater,
                                     private juce::AudioProcessorParameter::Listener {
  public:
    KhensuCopilotProcessor();
    ~KhensuCopilotProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void setNonRealtime(bool isNonRealtime) noexcept override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    juce::AudioProcessorParameter* getBypassParameter() const override;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    int getAbMode() const noexcept { return abMode_.load(); }
    /// The A/B mode the audio path actually uses: always Processed (0) while
    /// the host renders offline (PLG-12), so a mode left on A or Delta never
    /// leaks into a bounce.
    int getEffectiveAbMode() const noexcept { return isNonRealtime() ? 0 : abMode_.load(std::memory_order_relaxed); }
    /// Blind mode: true when the hidden trial is the processed side (reveal).
    bool isBlindTrialProcessed() const noexcept { return outputStage_.blindIsProcessed(); }
    /// How far the heard side is turned down under the limiter ceiling, in
    /// dB (<= 0; FIX-12). Non-zero means the A/B is not level matched now.
    float getMonitorTrimDb() const noexcept { return outputStage_.monitorTrimDb(); }
    /// Blind mode: draw a new hidden side for the next trial.
    void nextBlindTrial() noexcept { outputStage_.requestBlindReshuffle(); }
    /// Runs LONG editor-side background work (intake, chat, chain generation)
    /// on a pool the processor owns and joins in its destructor, so no job can
    /// outlive the plugin DLL the way detached juce::Thread::launch threads
    /// could. Long jobs must poll ThreadPoolJob::shouldExit() between short
    /// requests (see CopilotServiceJobs).
    void runInBackground(std::function<void()> job) { backgroundPool_.addJob(std::move(job)); }
    /// Fire-and-forget SHORT work (feedback writes, telemetry events) on its
    /// own pool (AUDIT-029), so it is never queued behind a minutes-long job.
    /// Jobs must bound every request by kShortJobTimeoutMs.
    void runShortInBackground(std::function<void()> job) { shortJobPool_.addJob(std::move(job)); }
    static constexpr int kShortJobTimeoutMs = 2500;
    void setAbMode(int mode) noexcept { abMode_.store(mode); }
    bool isServiceHealthy() const noexcept
    {
        return transportController_.serviceHealthStatus() == ipc::ServiceHealthStatus::Connected;
    }
    /// Richer status than isServiceHealthy() (audit ARCH-05): distinguishes
    /// "never seen the service" (the desktop app was never launched this
    /// session - the right action is to start it) from "was connected, now
    /// isn't" (the desktop app or service went away mid-session). The plugin
    /// never spawns/stops the service itself - it is a best-effort client of
    /// a desktop-owned service, and this only makes that existing dependency
    /// legible rather than changing its ownership.
    ipc::ServiceHealthStatus serviceHealthStatus() const noexcept { return transportController_.serviceHealthStatus(); }
    int serviceHealthConsecutiveFailures() const noexcept
    {
        return transportController_.serviceHealthConsecutiveFailures();
    }

    const juce::String getName() const override { return "Khensu Copilot"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    /// The longest tail of the hosted plugins that carry signal (AUDIT-062),
    /// so hosts that suspend silent plugins don't cut reverb/delay tails.
    /// Published by the message thread; lock-free here.
    double getTailLengthSeconds() const override { return hostedTailSeconds_.load(std::memory_order_relaxed); }
#if JucePlugin_Enable_ARA
    juce::AudioProcessorARAExtension* getARAClientExtensions() override { return this; }
#endif

    // ---- UI-9: factory presets as host programs ------------------------------
    int getNumPrograms() override { return copilot::presets::numFactoryPresets(); }
    int getCurrentProgram() override { return currentProgram_.load(std::memory_order_relaxed); }
    /// Loads factory preset `index` into every DSP parameter (the ones it
    /// does not name return to their defaults). No locks or allocation, so a
    /// host may call it from any thread.
    void setCurrentProgram(int index) override;
    const juce::String getProgramName(int index) override;
    void changeProgramName(int, const juce::String&) override {}

    /// Loads factory preset `index` as the user's choice: one undo step
    /// ("Preset: Club master"). THREAD_CONTEXT(UIThread).
    void loadProgramFromUi(int index);

    // ---- UI-9 / FN2-5: user presets (shared chain presets) and A/B snapshots --
    /// Name of the preset the parameters were last loaded from ("" = none).
    juce::String getCurrentPresetName() const;
    /// The shared per-user preset folder (<data dir>/presets, FN2-5), which
    /// the desktop rack reads too.
    juce::File getUserPresetDirectory() const;
    void setUserPresetDirectoryForTesting(const juce::File& dir) { userPresetDirOverride_ = dir; }
    /// Where pre-FN2-5 XML presets were kept (<data dir>/Copilot Presets).
    juce::File getLegacyUserPresetDirectory() const;
    void setLegacyUserPresetDirectoryForTesting(const juce::File& dir) { legacyPresetDirOverride_ = dir; }
    /// The user presets, re-read from the folder (after migrating any legacy
    /// ones), sorted by name. THREAD_CONTEXT(UIThread).
    std::vector<khensu::presets::ListedPreset> listUserPresets() const;
    /// Saves every DSP parameter as a user preset `name` (a desktop preset of
    /// that name keeps its desktop-only stages). THREAD_CONTEXT(UIThread).
    bool saveUserPreset(const juce::String& name, juce::String& error);
    /// Loads a user preset file (as host gestures, one undo step).
    /// Parameters it does not name return to their defaults; desktop-only
    /// stages are ignored. THREAD_CONTEXT(UIThread).
    bool loadUserPreset(const juce::File& file, juce::String& error);
    /// The two parameter snapshots: 0 = A, 1 = B. The active one IS the
    /// live parameters; the other is held until switched to.
    int getActiveSnapshot() const noexcept { return activeSnapshot_.load(std::memory_order_relaxed); }
    /// Keeps the live values as the active snapshot and loads the other one
    /// (one undo step; undo switches back). THREAD_CONTEXT(UIThread).
    void switchSnapshot(int slot);

    /// UX2-6b: the parameter undo history (message thread only).
    copilot::ParamUndoHistory& undoHistory() noexcept { return undoHistory_; }
    /// Copies the active snapshot over the other (A -> B when A is active).
    void copyActiveSnapshotToOther();

    // ---- UI-8: editor view, persisted per instance ----------------------------
    enum class ViewSize { Small = 0, Medium = 1, Large = 2 };
    struct EditorViewState {
        bool compact = false;
        ViewSize size = ViewSize::Medium;
        int tab = 0;
        bool operator==(const EditorViewState& o) const noexcept
        {
            return compact == o.compact && size == o.size && tab == o.tab;
        }
    };
    EditorViewState getEditorViewState() const noexcept;
    void setEditorViewState(const EditorViewState& state);

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    /// Checks a state blob before anything is restored (clap-validator
    /// state-invalid-random): size bounds, the binary-XML header (magic and
    /// declared length), a parseable XML root of this plugin's state type, a
    /// sane version, and well-formed PARAM nodes (known id, finite value). An
    /// invalid ValueTree with `reason` filled in means "reject untouched".
    juce::ValueTree validatedStateTree(const void* data, int sizeInBytes, juce::String& reason) const;
    /// Why the most recent setStateInformation() call was ignored, or "" when
    /// it was applied. Any thread: hosts restore state off the message thread,
    /// so this hands out a copy taken under a lock (FIX-18).
    juce::String lastStateRejection() const
    {
        const juce::ScopedLock lock(stateRejectionLock_);
        return lastStateRejection_;
    }
    /// Largest state blob setStateInformation() accepts.
    static constexpr int kMaxStateBytes = 256 * 1024 * 1024;

    juce::AudioProcessorValueTreeState& getAPVTS() { return apvts_; }

    // THREAD_CONTEXT(AnyThread) - atomic reads
    float getLeftPeakDb() const noexcept { return realtimeTelemetry_.snapshot().left_peak_db; }
    float getRightPeakDb() const noexcept { return realtimeTelemetry_.snapshot().right_peak_db; }
    float getMomentaryLufs() const noexcept { return realtimeTelemetry_.snapshot().momentary_lufs; }
    float getShortTermLufs() const noexcept { return realtimeTelemetry_.snapshot().short_term_lufs; }
    float getCorrelation() const noexcept { return realtimeTelemetry_.snapshot().correlation; }
    float getStereoWidth() const noexcept { return realtimeTelemetry_.snapshot().width; }
    float getIntegratedLufs() const noexcept { return realtimeTelemetry_.snapshot().integrated_lufs; }
    /// UI-5: restarts the integrated loudness (any thread).
    void resetIntegratedLoudness() noexcept { realtimeTelemetry_.requestIntegratedReset(); }
    /// UI-5: the highest true peak per channel since the previous call (UI thread).
    void takeTruePeaks(float& leftDbtp, float& rightDbtp) noexcept { realtimeTelemetry_.takeTruePeaks(leftDbtp, rightDbtp); }
    /// UI-5: the output for the vectorscope (audio thread fills, UI drains).
    ui::StereoSampleRing& getScopeRing() noexcept { return realtimeTelemetry_.scopeRing(); }
    /// OVH-8: the de-esser's gain reduction in the last block (dB).
    float getDeessReductionDb() const noexcept { return correctiveDsp_.lastDeessReductionDb(); }
    /// VIZ-6b: live gain reduction for the DSP tab's module drawings.
    float getCompReductionDb() const noexcept { return correctiveDsp_.lastCompReductionDb(); }
    float getCompInputDb() const noexcept { return correctiveDsp_.lastCompInputDb(); }
    float getLimiterReductionDb() const noexcept { return outputStage_.lastLimiterReductionDb(); }
    float getBpm() const noexcept { return realtimeTelemetry_.getBpm(); }
    double getLoopBeats() const noexcept { return realtimeTelemetry_.getLoopBeats(); }
    bool isHostLooping() const noexcept { return realtimeTelemetry_.isLooping(); }
    /// FEAT-7: the capture to a desktop session.
    copilot::CaptureRecorder& capture() noexcept { return capture_; }

    // Expose ring buffer for real-time metering and visualizations
    int readRingBuffer(juce::AudioBuffer<float>& dest) { return realtimeTelemetry_.readRingBuffer(dest); }

    int getRingBufferAvailable() const noexcept { return realtimeTelemetry_.getRingBufferAvailable(); }

    // ---- Cross-instance Mix hub (the "Mix" tab) ---------------------------------
    /// Host track name/colour changes: forwarded to this instance's hub frame.
    void updateTrackProperties(const TrackProperties& properties) override;
    /// This instance's id in the process-wide MixHub (stable for its lifetime).
    juce::uint64 getMixHubInstanceId() const noexcept { return mixHubTap_.getInstanceId(); }
    mixhub::MixHub& getMixHub() noexcept { return mixHubTap_.getHub(); }
    juce::String getHostTrackName() const { return mixHubTap_.getTrackName(); }
    /// The user's role choice for this instance in the Mix tab (PLG-18);
    /// empty = guessed. Persisted in the plugin state.
    juce::String getMixHubRoleOverride() const { return mixHubTap_.getRoleOverride(); }
    void setMixHubRoleOverride(const juce::String& role);

    /// Stable per-instance id (a GUID) that keys every /transport route
    /// (PLG-04). Persisted in the plugin state so it survives a project
    /// reload; a restored id already used by another live instance in this
    /// process (a duplicated track) is not adopted.
    juce::String getInstanceId() const;

    // THREAD_CONTEXT(UIThread)
    bool hasAnalysis() const noexcept;
    void getAnalysisSnapshot(AnalysisResult& outAnalysis, std::vector<Recommendation>& outRecs) const;

    // Dispatches background load; calls onLoaded on UIThread when complete (success or fail).
    void loadLastSessionAsync(std::function<void()> onLoaded);
    /// WF-8: the desktop session this instance follows ("" = the newest);
    /// saved with the plugin state.
    void setBoundSessionId(const std::string& id) { analysisController_.setBoundSessionId(id); }
    std::string boundSessionId() const { return analysisController_.boundSessionId(); }

    /// A finding's issue-queue state, shared with the desktop (CON-1).
    void setIssueState(const std::string& recId, issue_state::State state);
    issue_state::State issueState(const std::string& recId) const;
    std::string getSessionId() const;

    /// Last seek position requested by the desktop app via /transport/seek.
    /// VST3 gives plugins no way to move the host transport, so this is
    /// surfaced for the editor UI (e.g. drawing a seek marker) instead.

    using HostedPluginSlot = HostedPluginChainManager::Slot;
    using PersistedHostedPluginSlot = HostedPluginChainManager::PersistedSlot;

    void addPluginToChainAsync(const juce::String& filePath, std::function<void(bool)> onComplete = nullptr);
    /// Publishes an already-created instance as the last chain slot. The tail
    /// of addPluginToChainAsync after the format-manager load, exposed so
    /// deterministic tests can inject a mock instance (latency-bearing,
    /// destructor-counting) without touching the plugin-scanning path.
    /// THREAD_CONTEXT(UIThread). The chain manager configures the instance's
    /// buses and prepares it at the current rate/block size.
    /// Returns false (and drops the instance) for a Khensu Copilot instance:
    /// hosting the Copilot inside itself is refused (PLG-24).
    bool addPluginInstanceToChain(std::unique_ptr<juce::AudioPluginInstance> instance, const juce::String& name,
                                  const juce::String& filePath = {});
    /// True when `description` names this plugin (Khensu Copilot) itself.
    static bool isCopilotItself(const juce::PluginDescription& description);
    void removePluginFromChain(int slotIndex);
    /// Reorders the hosted chain (PLG-06). THREAD_CONTEXT(UIThread).
    bool moveHostedPlugin(int fromIndex, int toIndex);
    /// Opens (or brings to front) the editor window of hosted slot
    /// `slotIndex` (PLG-06). False for a placeholder / invalid index.
    /// THREAD_CONTEXT(UIThread).
    bool openHostedPluginEditor(int slotIndex);
    /// Closes the editor window of the slot with `slotId`, if open.
    void closeHostedPluginEditor(uint64_t slotId);
    int getNumOpenHostedEditors() const noexcept { return static_cast<int>(hostedEditorWindows_.size()); }
    bool isHostedEditorOpen(uint64_t slotId) const { return hostedEditorWindows_.count(slotId) > 0; }
    void clearHostedPluginChain();
    void setHostedPluginBypass(int slotIndex, bool bypassed);
    void setHostedPluginWetMix(int slotIndex, float wetMix);
    int getNumHostedPlugins() const noexcept;
    using HostedPluginSlotInfo = HostedPluginChainManager::SlotInfo;
    /// THREAD_CONTEXT(UIThread). Per-slot name/bypass/wet/latency for the
    /// editor's hosted-chain panel; takes the chain manager's control lock
    /// only (the audio thread never does), so it cannot stall processBlock.
    std::vector<HostedPluginSlotInfo> getHostedPluginSlots() const;
    /// True from setStateInformation() until every saved hosted slot has
    /// been restored (or replaced by a placeholder).
    bool isHostedRestorePending() const;

    // Parameter IDs for the built-in corrective DSP chain. Exposed so the
    // editor and MixerChainMiniPanel can set them by name.
    struct ParamIDs {
        static constexpr const char* hpfOn = "hpf_on";
        static constexpr const char* hpfFreq = "hpf_freq";
        static constexpr const char* lpfOn = "lpf_on";
        static constexpr const char* lpfFreq = "lpf_freq";
        static constexpr const char* eqOn = "eq_on";
        static constexpr const char* eqFreq = "eq_freq";
        static constexpr const char* eqGainDb = "eq_gain_db";
        static constexpr const char* eqQ = "eq_q";
        // Bands 2 and 3 of the corrective EQ stage - added so a generated or
        // approximated plugin recipe naming multiple EQ moves (e.g. a Pultec's
        // low boost + high boost, or a 3-band match-EQ) can land on 3 real
        // parametric bands instead of being collapsed onto a single one.
        static constexpr const char* eq2On = "eq2_on";
        static constexpr const char* eq2Freq = "eq2_freq";
        static constexpr const char* eq2GainDb = "eq2_gain_db";
        static constexpr const char* eq2Q = "eq2_q";
        static constexpr const char* eq3On = "eq3_on";
        static constexpr const char* eq3Freq = "eq3_freq";
        static constexpr const char* eq3GainDb = "eq3_gain_db";
        static constexpr const char* eq3Q = "eq3_q";
        static constexpr const char* compOn = "comp_on";
        static constexpr const char* compThreshDb = "comp_thresh_db";
        static constexpr const char* compRatio = "comp_ratio";
        static constexpr const char* compAttackMs = "comp_attack_ms";
        static constexpr const char* compReleaseMs = "comp_release_ms";
        static constexpr const char* compMultiband = "comp_multiband";
        static constexpr const char* compLowThreshDb = "comp_low_thresh_db";
        static constexpr const char* compMidThreshDb = "comp_mid_thresh_db";
        static constexpr const char* compHighThreshDb = "comp_high_thresh_db";
        static constexpr const char* crossoverLowFreq = "crossover_low_freq";
        static constexpr const char* crossoverHighFreq = "crossover_high_freq";
        static constexpr const char* outGainDb = "out_gain_db";
        static constexpr const char* limOn = "lim_on";
        static constexpr const char* limThreshDb = "lim_thresh_db";
        static constexpr const char* chainMix = "chain_mix";
        static constexpr const char* bypass = "bypass";
        // OVH-8 (state v3): appended to the layout, so v2 projects load with
        // these at their defaults. Same strings as mixerchain::DspParamIdsV3.
        static constexpr const char* eqType = "eq_type";   ///< 0 bell, 1 low shelf, 2 high shelf
        static constexpr const char* eq2Type = "eq2_type";
        static constexpr const char* eq3Type = "eq3_type";
        static constexpr const char* deessOn = "deess_on";
        static constexpr const char* deessFreq = "deess_freq";
        static constexpr const char* deessThreshDb = "deess_thresh_db";
        static constexpr const char* deessRangeDb = "deess_range_db";
        /// The Apply-top-fix Amount, 0..1: re-applies the live fix as it moves.
        static constexpr const char* fixAmount = "fix_amount";
    };

    // The built-in DSP-chain params that the AI mixer-chain generator or
    // suggestion auto-apply can write. chain_mix and bypass are chain-level
    // controls, not individual DSP stages, so they're excluded and never get
    // an AI-set dot.
    static constexpr int kNumDspParams = 37;
    static constexpr std::array<const char*, kNumDspParams> kDspParamIds{{ParamIDs::hpfOn,
                                                                          ParamIDs::hpfFreq,
                                                                          ParamIDs::lpfOn,
                                                                          ParamIDs::lpfFreq,
                                                                          ParamIDs::eqOn,
                                                                          ParamIDs::eqFreq,
                                                                          ParamIDs::eqGainDb,
                                                                          ParamIDs::eqQ,
                                                                          ParamIDs::eq2On,
                                                                          ParamIDs::eq2Freq,
                                                                          ParamIDs::eq2GainDb,
                                                                          ParamIDs::eq2Q,
                                                                          ParamIDs::eq3On,
                                                                          ParamIDs::eq3Freq,
                                                                          ParamIDs::eq3GainDb,
                                                                          ParamIDs::eq3Q,
                                                                          ParamIDs::compOn,
                                                                          ParamIDs::compThreshDb,
                                                                          ParamIDs::compRatio,
                                                                          ParamIDs::compAttackMs,
                                                                          ParamIDs::compReleaseMs,
                                                                          ParamIDs::compMultiband,
                                                                          ParamIDs::compLowThreshDb,
                                                                          ParamIDs::compMidThreshDb,
                                                                          ParamIDs::compHighThreshDb,
                                                                          ParamIDs::crossoverLowFreq,
                                                                          ParamIDs::crossoverHighFreq,
                                                                          ParamIDs::outGainDb,
                                                                          ParamIDs::limOn,
                                                                          ParamIDs::limThreshDb,
                                                                          ParamIDs::eqType,
                                                                          ParamIDs::eq2Type,
                                                                          ParamIDs::eq3Type,
                                                                          ParamIDs::deessOn,
                                                                          ParamIDs::deessFreq,
                                                                          ParamIDs::deessThreshDb,
                                                                          ParamIDs::deessRangeDb}};

    // ---- OVH-8: the live top fix ------------------------------------------------
    struct LiveFixResult {
        mixerchain::MappedFix mapped; ///< what the fix maps to at the current amount
        bool applied = false;         ///< false when blocked or nothing mapped
        juce::String blockedReason;   ///< why it was not applied ("" when applied)
    };
    /// Why `rec` is not applied automatically ("" when safe_to_auto_apply).
    static juce::String autoApplyBlockReason(const Recommendation& rec);
    /// Applies `rec` to the built-in chain at the current fix_amount (as host
    /// gestures) and keeps it live: moving fix_amount re-applies it over the
    /// values the parameters had before the fix. With `requireSafe`, a finding
    /// not marked safe_to_auto_apply is only mapped, not applied.
    /// THREAD_CONTEXT(UIThread).
    LiveFixResult applyLiveFix(const Recommendation& rec, bool requireSafe = true);
    /// Re-applies the live fix at the current fix_amount. THREAD_CONTEXT(UIThread).
    void reapplyLiveFix();
    /// Forgets the live fix (the parameters stay as they are).
    void clearLiveFix();
    bool hasLiveFix() const noexcept { return liveFixActive_.load(std::memory_order_acquire); }
    std::string getLiveFixId() const;
    float getFixAmount() const noexcept { return pFixAmount_ != nullptr ? pFixAmount_->load(std::memory_order_relaxed) : 1.0f; }

    /// Set a parameter by ID from the UI thread (normalises and notifies host).
    void setParamFromUi(const char* paramId, float plainValue);

    /// Set a parameter from any thread. The DSP-facing APVTS atomic is updated
    /// immediately; host notification is queued to the message thread with no
    /// change gesture. Used by the sync worker applying desktop-pushed params.
    void setParamThreadSafe(const char* paramId, float plainValue);

    /// Applies a complete AI-generated parameter set (Mixer Chain "Apply",
    /// PLG-19). Every stage switch the set does not mention is turned OFF and
    /// the output gain returns to 0 dB unless the set names it, so stages
    /// from a previous Apply no longer stay on. THREAD_CONTEXT(UIThread).
    void applyAiParameterSet(const std::map<std::string, float>& params);

    /// Moves the output gain by `deltaDb` relative to its current value
    /// (suggestion keys reduce_gain_db / increase_gain_db / makeup are
    /// relative, PLG-19) and marks it AI-set. THREAD_CONTEXT(UIThread).
    void adjustOutGainFromAi(float deltaDb);

    /// Set a parameter from the UI thread AND mark it as AI-authored (lights
    /// the "AI-set" dot in DspControlGridPanel until the user manually edits
    /// that same control). Used by MixerChainMiniPanel::applyChainToProcessor()
    /// and the suggestion-feed auto-apply path - never by direct user input.
    void setParamFromAI(const char* paramId, float plainValue);

    /// True if paramId's current value was last written by setParamFromAI
    /// and hasn't been manually edited since. False for any ID not in
    /// kDspParamIds (i.e. chain_mix, bypass never report true).
    bool isAiSet(const char* paramId) const noexcept;

    /// Clears the AI-set flag for paramId. Also cleared automatically by ANY
    /// value change that did not come from setParamFromAI (keyboard, typed
    /// value, host automation, a desktop push - PLG-20).
    void clearAiSet(const char* paramId) noexcept;

    // ---- Timed findings, host seeking and fixes as automation (CE-DAW-1/3/4) ----
    /// Live findings of this instance (rolling analysis of its output).
    timeline::IssueTimeline& getIssueTimeline() noexcept { return issueTimeline_; }
    live::CopilotLiveAnalyzer& getLiveAnalyzer() noexcept { return liveAnalyzer_; }
    void setLiveAnalysisEnabled(bool enabled) noexcept { liveAnalyzer_.setEnabled(enabled); }
    bool isLiveAnalysisEnabled() const noexcept { return liveAnalyzer_.isEnabled(); }

    /// The ARA document this instance is bound to (KhensuCopilotARA in an ARA
    /// host), else nullptr. THREAD_CONTEXT(UIThread).
    ara::IAraSession* getAraSession() noexcept;

    /// How seekHostTo() moved (or asked to move) the host playhead.
    enum class SeekPath { Ara, ServiceMmc };
    /// THREAD_CONTEXT(UIThread). Jump-to-issue: through the ARA playback
    /// controller when the host provides one, else through the service's
    /// /transport/seek, which polls into every instance and sends an MMC
    /// Locate on the Khensu MIDI port (the host must follow MMC).
    SeekPath seekHostTo(double timeSec);

    /// THREAD_CONTEXT(UIThread). Arms "Write fix as automation": the issue's
    /// suggested parameters become ramped curves over its time span, played
    /// on this plugin's own parameters inside change gestures while the host
    /// plays through it (put the host's automation in write/touch/latch).
    /// False (with `message`) when nothing in the fix maps to a parameter.
    bool armFixAutomation(const timeline::TimedIssue& issue, juce::String& message);
    void cancelFixAutomation();
    automation::FixAutomationWriter::State getFixAutomationState() const noexcept { return fixWriter_.state(); }
    const automation::FixCurve& getFixAutomationCurve() const noexcept { return fixWriter_.curve(); }
    double getFixAutomationProgress() const noexcept { return fixWriter_.progress(); }
    /// Plain value of a parameter by id (0 for an unknown id).
    float getParamPlain(const std::string& paramId) const;

    /// Root plugin-state version written by getStateInformation (PLG-23).
    /// 1 = legacy (no version property); 2 = adds instance id and Mix role;
    /// 3 = adds the EQ shapes, the de-esser and fix_amount (new parameters,
    /// default when absent), the editor view, the program / preset name and
    /// the A/B snapshot (UI-8, UI-9, OVH-8).
    static constexpr int kStateVersion = 3;
    /// Version of the embedded hosted-chain node.
    static constexpr int kHostedChainVersion = 1;
    /// Upgrades a state tree saved by an older version, in place. The single
    /// migration hook every future state change goes through.
    static void migrateState(juce::ValueTree& state, int fromVersion);

    /// Tells the host the project changed outside the parameters (hosted
    /// chain edits, AI flags, role, ...) so it prompts to save (PLG-05).
    /// Any thread: off the message thread it is deferred to it.
    void markNonParameterStateChanged() noexcept;

  private:
    juce::AudioProcessorValueTreeState apvts_;
    /// UX2-6b: after apvts_, whose parameters it listens to.
    copilot::ParamUndoHistory undoHistory_{*this};
    CopilotRealtimeTelemetry realtimeTelemetry_;
    copilot::CaptureRecorder capture_; ///< FEAT-7

    // ---- Built-in corrective DSP chain ----------------------------------------
    CopilotCorrectiveDsp correctiveDsp_;
    double currentSampleRate_ = 44100.0;
    int preparedChannels_ = 2;

    // ---- Post-chain output stage (lookahead limiter + A/B/Delta) --------------
    CopilotOutputStage outputStage_;

    // Cached raw parameter pointers (owned by apvts_)
    std::atomic<float>* pBypass_ = nullptr;
    std::atomic<float>* pChainMix_ = nullptr;
    std::atomic<float>* pHpfOn_ = nullptr;
    std::atomic<float>* pHpfFreq_ = nullptr;
    std::atomic<float>* pLpfOn_ = nullptr;
    std::atomic<float>* pLpfFreq_ = nullptr;
    std::atomic<float>* pEqOn_ = nullptr;
    std::atomic<float>* pEqFreq_ = nullptr;
    std::atomic<float>* pEqGainDb_ = nullptr;
    std::atomic<float>* pEqQ_ = nullptr;
    std::atomic<float>* pEq2On_ = nullptr;
    std::atomic<float>* pEq2Freq_ = nullptr;
    std::atomic<float>* pEq2GainDb_ = nullptr;
    std::atomic<float>* pEq2Q_ = nullptr;
    std::atomic<float>* pEq3On_ = nullptr;
    std::atomic<float>* pEq3Freq_ = nullptr;
    std::atomic<float>* pEq3GainDb_ = nullptr;
    std::atomic<float>* pEq3Q_ = nullptr;
    std::atomic<float>* pCompOn_ = nullptr;
    std::atomic<float>* pCompThreshDb_ = nullptr;
    std::atomic<float>* pCompRatio_ = nullptr;
    std::atomic<float>* pCompAttackMs_ = nullptr;
    std::atomic<float>* pCompReleaseMs_ = nullptr;
    std::atomic<float>* pCompMultiband_ = nullptr;
    std::atomic<float>* pCompLowThreshDb_ = nullptr;
    std::atomic<float>* pCompMidThreshDb_ = nullptr;
    std::atomic<float>* pCompHighThreshDb_ = nullptr;
    std::atomic<float>* pCrossoverLowFreq_ = nullptr;
    std::atomic<float>* pCrossoverHighFreq_ = nullptr;
    std::atomic<float>* pOutGainDb_ = nullptr;
    std::atomic<float>* pLimOn_ = nullptr;
    std::atomic<float>* pLimThreshDb_ = nullptr;
    std::atomic<float>* pEqType_ = nullptr;
    std::atomic<float>* pEq2Type_ = nullptr;
    std::atomic<float>* pEq3Type_ = nullptr;
    std::atomic<float>* pDeessOn_ = nullptr;
    std::atomic<float>* pDeessFreq_ = nullptr;
    std::atomic<float>* pDeessThreshDb_ = nullptr;
    std::atomic<float>* pDeessRangeDb_ = nullptr;
    std::atomic<float>* pFixAmount_ = nullptr;
    int fixAmountParamIndex_ = -1;

    // ---- OVH-8 live fix (message thread, except the flags) ----
    Recommendation liveFix_;
    std::map<std::string, float> liveFixBase_; ///< plain values before the fix
    std::atomic<bool> liveFixActive_{false};
    std::atomic<bool> pendingFixReapply_{false};
    float readPlain(const char* paramId) const noexcept;

    // ---- UI-9 programs / presets / snapshots ----
    std::atomic<int> currentProgram_{0};
    /// Per factory preset, every snapshot parameter's normalised value -
    /// resolved in the constructor so setCurrentProgram never allocates.
    std::vector<std::vector<std::pair<juce::RangedAudioParameter*, float>>> programValues_;
    /// The parameters a preset or snapshot covers: every DSP id + chain mix.
    std::vector<juce::RangedAudioParameter*> snapshotParams_;
    juce::String presetName_;
    mutable juce::CriticalSection presetLock_; ///< guards presetName_ and otherSnapshot_
    std::atomic<int> activeSnapshot_{0};
    std::vector<float> otherSnapshot_; ///< plain values of snapshotParams_; empty = not taken yet
    juce::File userPresetDirOverride_;
    juce::File legacyPresetDirOverride_;
    void switchSnapshotNow(int slot);
    std::vector<float> captureSnapshot() const;
    void loadSnapshot(const std::vector<float>& plainValues);
    juce::String snapshotToString(const std::vector<float>& plainValues) const;
    std::vector<float> snapshotFromString(const juce::String& text) const;

    // ---- UI-8 editor view ----
    std::atomic<bool> viewCompact_{false};
    std::atomic<int> viewSize_{1};
    std::atomic<int> viewTab_{0};

    // One flag per entry in kDspParamIds - true while that param's current
    // value was set by the AI (chain-gen or suggestion auto-apply) and has
    // not been manually edited since. Persisted via getStateInformation/
    // setStateInformation as a comma-separated list of param IDs (a JUCE
    // APVTS ValueTree property, not part of the parameter tree itself), so
    // it now survives a project reload. Atomic (AUDIT-085): the UI writes
    // them, but hosts may call get/setStateInformation off the message
    // thread. Explicitly initialised in the constructor - a value-initialised
    // std::array of atomics is not guaranteed zeroed in C++17.
    std::array<std::atomic<bool>, kNumDspParams> aiSetFlags_;
    /// AudioProcessor parameter index -> kDspParamIds index (or -1); built in
    /// the constructor so the parameter listener never allocates.
    std::vector<int> parameterToDspIndex_;
    std::atomic<bool> pendingNonParameterChange_{false};
    juce::String lastStateRejection_; ///< see lastStateRejection(); guarded by stateRejectionLock_
    mutable juce::CriticalSection stateRejectionLock_;
    std::atomic<bool> restoringState_{false}; ///< inside setStateInformation: no dirty marking

    static int dspParamIndex(const char* paramId) noexcept;
    void parameterValueChanged(int parameterIndex, float newValue) override;
    void parameterGestureChanged(int, bool) override {}
    bool setAiFlag(int index, bool value) noexcept; ///< true when it changed

    mutable juce::CriticalSection instanceIdLock_;
    juce::String instanceId_;
    juce::String persistedInstanceId_; ///< id a restored state carried (written back verbatim)
    void adoptInstanceId(const juce::String& restoredId);

    std::atomic<bool> preparedOnce_{false}; // gates transport sync until real audio use

    // ---- Transport sync worker ------------------------------------------------
    // Owned, joined thread (stopThread in the processor destructor). Replaces
    // the previous Timer + detached juce::Thread::launch pattern, which had a
    // check-then-use race on `alive_` and could write freed processor memory.
    // One bridge instance for the thread's lifetime also means the auth token
    // is read from disk once, not once per tick.
    // ---- Transport sync worker ------------------------------------------------
    // The controller owns the thread, bridge, health state, and polling
    // cadence. These providers keep audio state in the processor without
    // exposing the processor as the worker's broad dependency.
    CopilotTransportController transportController_{
        [this] { return preparedOnce_.load(std::memory_order_acquire); },
        [this] {
            const auto telemetry = realtimeTelemetry_.snapshot();
            ipc::HttpServiceBridge::PlayheadState state;
            state.position_sec = telemetry.position_sec;
            state.is_playing = telemetry.is_playing;
            state.left_peak_db = telemetry.left_peak_db;
            state.right_peak_db = telemetry.right_peak_db;
            state.momentary_lufs = telemetry.momentary_lufs;
            state.short_term_lufs = telemetry.short_term_lufs;
            state.correlation = telemetry.correlation;
            state.width = telemetry.width;
            return state;
        },
        // No seek sink: a plugin cannot move the host's playhead, and the
        // value it used to keep was never read (CLN-1).
        {},
        [this](const juce::String& paramId, float value) { setParamThreadSafe(paramId.toRawUTF8(), value); }};

    std::atomic<int> abMode_{0};
    /// Hosted plugin editor windows by slot id (PLG-06). Message thread only.
    std::map<uint64_t, std::unique_ptr<HostedPluginEditorWindow>> hostedEditorWindows_;
    void closeAllHostedEditors();
    juce::ThreadPool backgroundPool_{2};
    juce::ThreadPool shortJobPool_{1};
    std::atomic<int> reportedHostedLatency_{0}; ///< hosted latency included in getLatencySamples()
    std::atomic<double> hostedTailSeconds_{0.0};
    /// Last flag passed to setNonRealtime (early-out; the VST3 wrapper calls it every block).
    std::atomic<bool> nonRealtimeFlag_{false};
    /// Prepared maximum block size; larger host blocks are split (AUDIT-027).
    int preparedBlockSize_ = 0;
    void processBlockInternal(juce::AudioBuffer<float>& buffer, bool globalBypass);
    void processChunk(juce::AudioBuffer<float>& buffer, bool globalBypass,
                      const CopilotRealtimeTelemetry::HostTransport& transport);
    void handleAsyncUpdate() override;
    static bool resolvePluginDescription(juce::AudioPluginFormatManager& formatManager,
                                         const juce::String& filePath, juce::PluginDescription& out);

    void updateLatency();
    void restoreHostedChainAsync(std::vector<PersistedHostedPluginSlot> slots);
    void restoreNextHostedSlotAsync(std::shared_ptr<const std::vector<PersistedHostedPluginSlot>> slots,
                                    size_t index, uint64_t generation);
    void finishHostedRestore(uint64_t generation);

    HostedPluginChainManager hostedPlugins_;
    std::atomic<uint64_t> hostedRestoreGeneration_{0};
    /// The slots an in-flight restore is working from; saved verbatim by
    /// getStateInformation() until the restore completes (audit M5).
    std::shared_ptr<const std::vector<PersistedHostedPluginSlot>> pendingHostedRestore_;
    juce::CriticalSection pendingHostedRestoreLock_;
    CopilotAnalysisController analysisController_;

    // CE-DAW-3: the timeline outlives the live worker that writes it.
    timeline::IssueTimeline issueTimeline_;
    live::CopilotLiveAnalyzer liveAnalyzer_{issueTimeline_};

    // CE-DAW-4: "Write fix as automation" - message-thread writer driven at
    // 30 Hz by its own timer only while a curve is armed.
    struct GestureSink final : automation::IGestureSink {
        explicit GestureSink(KhensuCopilotProcessor& p) : owner(p) {}
        bool beginGesture(const std::string& paramId) override;
        void setValue(const std::string& paramId, float plainValue) override;
        void endGesture(const std::string& paramId) override;
        KhensuCopilotProcessor& owner;
    };
    GestureSink gestureSink_{*this};
    automation::FixAutomationWriter fixWriter_;
    juce::TimedCallback fixAutomationTimer_{[this] { tickFixAutomation(); }};
    void tickFixAutomation();
    /// Publishes this track's band/loudness frame to the in-process MixHub.
    /// Audio thread: FIFO write + atomics only (see mixhub/MixHub.h).
    mixhub::MixHubTap mixHubTap_;

    // ---- Retirement reaper (audit ARCH-04) ------------------------------------
    // Retired chain snapshots used to be reclaimed only inside the NEXT chain
    // mutation, so repeated add/remove/clear while the transport was stopped
    // (epoch frozen) could retain full third-party instances indefinitely.
    // A message-thread timer now frees them on two independent conditions:
    //   1. the audio epoch has advanced past the entry's releaseAfterEpoch
    //      (a processBlock has completed since retirement), or
    //   2. quiescence: the epoch has not moved across a full reaper period AND
    //      the entry was retired before the previous tick - no audio callback
    //      ran in that window, so nothing can still be executing the snapshot.
    // Destruction happens on the message thread (the reaper's own thread).
    // The manager owns the design occupancy bound and release policy; this
    // timer only gives it its message-thread execution point.
    static constexpr int kReaperPeriodMs = 500;
    void timerCallback() override;
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    /// Created once in the constructor on the message thread and copied by
    /// every async path (audit L12) - see the constructor.
    juce::WeakReference<KhensuCopilotProcessor> weakThis_;

    JUCE_DECLARE_WEAK_REFERENCEABLE(KhensuCopilotProcessor)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(KhensuCopilotProcessor)
};

} // namespace khensu
