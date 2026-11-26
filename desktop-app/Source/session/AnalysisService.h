// desktop-app/Source/session/AnalysisService.h
#pragma once
#include <JuceHeader.h>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <khensu/targets/GenreDatabase.h>
#include <khensu/systembrain/SystemBrainDatabase.h>
#include "../services/IAnalysisHub.h"
#include "../analysis/FixVerification.h"
#include "../analysis/ServiceReadyWait.h"
#include "../ui/shared/PythonStatus.h"
#include "AnalysisResultModel.h"

namespace khensu {

class IBackgroundWork;
class SessionService;
class AnalysisController;
struct AnalysisServiceCalls;

/// The session's analysis and the results gate (ENG-1 stage C5, extracted
/// from AppController.cpp / AppController_Playback.cpp / _Session.cpp). Owns
/// the AnalysisController, the published-result model, the genre and System
/// Brain databases, the results-gate state machine (Analyzing -> Open on the
/// final result, or Cancelled / Failed), the follow-up scan tokens and count
/// (not waited for, PERF-1), the run token, the pipeline-progress and service-recovered
/// listeners, the reference track and saving each final result to the store.
///
/// The gate rules (CLAUDE.md "Results gate"): a partial result is never
/// published; the final result is published once, the Dashboard registers
/// its follow-up scans inside that broadcast, and the batch token is read
/// AFTER the broadcast.
///
/// FN2-2b: with the engine not running yet (a first launch) the main
/// pipeline's final result is the native one - complete, with the engine's
/// inputs marked unanswered (inputs_failed) - and it is published at once.
/// When the engine is Running the analysis is re-run IN PLACE: the gate
/// stays open and the native result stays on screen until the full result
/// supersedes it (docs/architecture.md, "Results gate"). Everything on screen (overlay, sidebar status, intake
/// HUD, health strip, failure box) goes through the hooks below, which the
/// owner wires.
///
/// Message thread, except the save job it queues on `work`. Results hop back
/// from the AnalysisController's pool on the message thread and are dropped
/// once the controller is gone; the follow-up timeout holds a WeakReference.
/// The owner must drain `work` before destroying this (AppController's
/// BackgroundRunner does).
class AnalysisService final : public IAnalysisHub {
  public:
    /// Results gate. Findings and grades stay hidden while it is not Open.
    /// It waits only on what a grade is built from - the main analysis -
    /// not on the Dashboard's follow-up scans, which fill in afterwards
    /// (PERF-1).
    enum class ResultsGate { Open, Analyzing, Cancelled, Failed };

    AnalysisService(IBackgroundWork& work, SessionService& sessions);
    ~AnalysisService() override;

    // ---- Setup (owner, once, in this order) ----
    /// Loads the bundled genre norms / System Brain encyclopedia; a missing
    /// file leaves that database empty (the pickers and panel degrade).
    void loadReferenceData(const juce::File& genreFile, const juce::File& systemBrainFile);
    /// The shared feedback store; creates the AnalysisController (which
    /// adapts recommendations to learned preferences from it). May be null.
    void attachFeedbackManager(std::shared_ptr<FeedbackManager> feedback);

    // ---- Hooks (wired by the owner; message thread) ----
    /// The gate changed (not fired by resetForSession()). The owner updates
    /// the overlay mode, health strip, status label and pending stem start,
    /// then its overlay visibility.
    std::function<void(ResultsGate gate)> onGateChanged;
    /// Sidebar analysis status ("Analyzing...", "Deep analysis...", ...).
    std::function<void(const juce::String& text, bool ok, bool inProgress)> onStatus;
    /// A pipeline stage of the running analysis, before the pipeline-progress
    /// listeners hear it (intake HUD; the overlay while Analyzing).
    std::function<void(const std::string& stage, int idx, int total)> onStage;
    /// The run failed (after the gate went to Failed): overlay text + message box.
    std::function<void(const juce::String& message)> onFailed;
    /// Follow-up scans pending / total of the current batch. They no longer
    /// hold the gate; the owner shows them as background progress.
    std::function<void(int pending, int total)> onFollowUpsChanged;
    /// The fast pass of the running analysis (loudness, true peak,
    /// clipping): shown at once, marked provisional. Never broadcast to the
    /// analysis listeners - its grades would move (PERF-1).
    std::function<void(const AnalysisResult& fast)> onProvisionalResult;
    /// The final result was just published (intake HUD done).
    std::function<void()> onFinalResult;
    /// The user cancelled a running analysis (intake HUD reset).
    std::function<void()> onCancelled;
    /// setReferenceTrack() changed the reference (the owner autosaves).
    std::function<void()> onReferenceChanged;
    /// WF-6: re-run only the reference comparison for `path` and republish.
    void refreshReferenceComparison(const juce::String& path);

    // ---- IAnalysisHub ----
    AnalysisResultModel& analysisModel() noexcept override { return analysisModel_; }
    void addAnalysisListener(void* key, std::function<void(const AnalysisResult&)> cb) override
    {
        analysisModel_.addListener(key, std::move(cb));
    }
    void removeAnalysisListener(void* key) override { analysisModel_.removeListener(key); }
    bool hasAnalysis() const noexcept override;
    const AnalysisResult& currentAnalysis() const noexcept override;
    uint64_t analysisVersion() const noexcept override { return analysisModel_.version(); }
    void recordScanResult(const juce::String& scan, const juce::var& result) override;
    juce::var scanResults() const override;
    /// Recommendations + MixReport + Scorecard for the current final result,
    /// computed once per result and shared (§10.3). `weightProfileId` is the
    /// ScoreWeightProfile genre id ("default" for the stock weights); empty
    /// means the session genre's profile.
    const ResultDerivedData& derivedResultData(const std::string& weightProfileId = {}) override;
    /// Merges a masking analysis result (run on demand from the Stems card)
    /// into the current result and rebroadcasts it, so the recommendations
    /// regenerate with MaskingRecommender's output included.
    void setStemMaskingResult(const MaskingSummary& masking) override;
    void setScoreWeightProfile(const std::string& profileId) override { scoreWeightProfile_ = profileId; }
    std::string scoreWeightProfile() const override { return scoreWeightProfile_; }
    DeepPassState deepPassState() const override;

    /// True while results are withheld (the main analysis is running).
    bool resultsPending() const noexcept override { return resultsGate_ != ResultsGate::Open; }
    /// Changes on every beginAnalysis() (a new run of the main pipeline) and
    /// every session change. The Dashboard keys its auto-scans on this, not
    /// on the file path: a re-analysis of the same file (service recovered,
    /// manual re-run, reopened session) re-runs them too (DSK-03).
    int analysisRunToken() const noexcept override { return analysisRunToken_; }
    /// A new batch of follow-up scans; finishes from an older batch are ignored.
    int startFollowUpScanBatch() noexcept override { return ++followUpToken_; }
    /// One more follow-up scan in batch `token` is running.
    void addFollowUpScan(int token) override;
    /// One follow-up scan of batch `token` finished (success, error or dropped).
    void followUpScanFinished(int token) override;
    /// Stop the analysis: nothing is published; the gate offers a re-run.
    void cancelAnalysis() override;
    /// Force a fresh full analysis of the active session (bypasses the result
    /// cache) - the Dashboard's manual "Re-run Deep Analysis" and the gate's
    /// re-run button.
    void reanalyzeActiveSession() override;
    /// Fired when the service comes back healthy and the current analysis
    /// already has its deep half - so nothing re-analyses, but scans that
    /// failed while it was down should re-run (DSK-03).
    void addServiceRecoveredListener(void* key, std::function<void()> cb) override
    {
        serviceRecoveredListeners_[key] = std::move(cb);
    }
    void removeServiceRecoveredListener(void* key) override { serviceRecoveredListeners_.erase(key); }
    /// Every pipeline stage transition - fast/deep analysis stages 1-5 and
    /// stems (stage 6, via notifyPipelineProgress()).
    void addPipelineProgressListener(void* key,
                                     std::function<void(const std::string& stage, int idx, int total)> cb) override
    {
        pipelineProgressListeners_[key] = std::move(cb);
    }
    void removePipelineProgressListener(void* key) override { pipelineProgressListeners_.erase(key); }

    /// The reference used for auto-comparison in each analysis; fires
    /// onReferenceChanged.
    void setReferenceTrack(const juce::String& path) override;
    juce::String getReferenceTrack() const override; // empty if none loaded
    bool genreRelativeActionAllowed() const override;
    juce::String genreRelativeAbstainedReason() const override;
    /// Session Intake hands over the deep-analysis response it computed for
    /// auto-populate; the first analysis of the same unmodified file reuses it.
    void cacheIntakeAnalysis(const juce::String& path, ipc::DeepAnalysisResponse response) override;

    const GenreDatabase& genreDatabase() const noexcept override { return genreDb_; }
    const GenreSpread& genreSpread() const noexcept override { return genreSpread_; }
    const SystemBrainDatabase& systemBrainDatabase() const noexcept override { return systemBrainDb_; }
    FeedbackManager* getFeedbackManager() const noexcept override { return feedback_.get(); }

    // ---- Owner API (AppController) ----
    /// Starts an analysis of the active session (no-op without one) and
    /// withholds the results until it and its follow-up scans finish.
    void beginAnalysis(bool forceRerun = false);
    /// The service's unhealthy->healthy edge (FIX-2). Re-runs the analysis
    /// only when its result records an input the engine was asked for and
    /// did not answer (AnalysisResult::inputs_failed). The re-run is in place
    /// (FN2-2b): the results stay on screen and the gate open until the new
    /// final result replaces them. Not one-shot, but bounded: the first
    /// re-run after a beginAnalysis() comes whenever the engine is there,
    /// each later one only after the engine came up again (a new
    /// unhealthy -> healthy edge), and at most kMaxRecoveryReruns per
    /// beginAnalysis() - a flapping engine must not re-analyse forever. A
    /// fix check that went unanswered is repeated once per render. A
    /// cancelled or failed analysis, and one whose deep pass was switched
    /// off, are left alone. Otherwise tells the service-recovered listeners
    /// to repeat their scans.
    void reanalyzeIfServiceRecovered();
    static constexpr int kMaxRecoveryReruns = 3;
    /// OVH-1: the lifecycle's status, the one source for "is the engine
    /// there". The deep pass reads it (EngineGate) instead of its own
    /// probe - and does not wait while it says Starting or Down (FN2-2b);
    /// and whenever the engine is Running, a result whose engine inputs went
    /// unanswered is re-run in place (the rule above) - not only on an
    /// unhealthy -> healthy edge, which a busy-but-running engine never
    /// produced.
    void setEngineStatus(PythonStatus status);
    /// True while an in-place re-run (FN2-2b) is upgrading the result on screen.
    bool upgradeInFlight() const noexcept { return upgradeInFlight_; }
    std::shared_ptr<const service_ready::EngineGate> engineGate() const noexcept { return engineGate_; }
    /// Sets the gate and fires onGateChanged (the missing-file / no-files
    /// paths of loading a session).
    void setResultsGate(ResultsGate gate);
    ResultsGate resultsGate() const noexcept { return resultsGate_; }
    int followUpPending() const noexcept { return followUpPending_; }
    /// True between beginAnalysis() and its final / failed result.
    bool analysisInFlight() const noexcept { return analysisInFlight_; }
    /// Fans a stage out to the pipeline-progress listeners (the stems stage).
    void notifyPipelineProgress(const std::string& stage, int idx, int total);
    /// Session change (DSK-02): clears the reference (without
    /// onReferenceChanged), cancels and forgets the analysis, advances the run
    /// and follow-up tokens and opens the gate without firing onGateChanged.
    void resetForSession();

    // ---- Fix -> verify (FEAT-1) ----
    void verifyRender(const juce::String& renderPath) override;
    bool verificationRunning() const noexcept override { return verifyingPath_.isNotEmpty(); }
    const fix_verification::Result* currentVerification() const noexcept override;
    void addVerificationListener(void* key, std::function<void()> cb) override
    {
        verificationListeners_[key] = std::move(cb);
    }
    void removeVerificationListener(void* key) override { verificationListeners_.erase(key); }
    /// The sidebar status while a check runs / when it lands.
    std::function<void(const juce::String& text, bool inProgress)> onVerificationStatus;

    // ---- Test seam (tests/ui/AnalysisServiceTest.cpp only) ----
    /// Feeds `r` through the handler a running analysis of the active session
    /// delivers to, as the AnalysisController would; a final, non-failed
    /// result first becomes the controller's current result, as a finished
    /// run's does. Lets the gate be driven without the pipeline or a service.
    /// `forSessionId` overrides the session the result claims to belong to
    /// (empty = the active one), as a run started before a switch would.
    void deliverResultForTesting(const AnalysisResult& r, const std::string& forSessionId = {});
    /// Test seam: a run is started (gate, token, status) but the pipeline is
    /// not; results come through deliverResultForTesting(). Not for
    /// production code.
    void skipPipelineForTesting() noexcept { skipPipelineForTesting_ = true; }
    /// Test seam: the main analysis talks to `calls` instead of the local
    /// service (after attachFeedbackManager()). Not for production code.
    void setServiceCallsForTesting(const AnalysisServiceCalls& calls);
    /// Feeds a stage through the running analysis's progress handler.
    void deliverStageForTesting(const std::string& stage, int idx, int total);
    /// Feeds the analysis of the render being verified, as its controller would.
    void deliverVerificationForTesting(const AnalysisResult& renderResult);

  private:
    juce::DynamicObject::Ptr scanResults_; ///< WF-14
    void handleUpdate(const std::string& sessionId, const std::string& fileId, const AnalysisResult& r,
                      const std::string& sourcePath = {});
    void handleStage(const std::string& stage, int idx, int total);
    void handleVerification(juce::String renderPath, const AnalysisResult& r); // by value: may alias verifyingPath_
    void notifyVerification();

    IBackgroundWork& work_;
    SessionService& sessions_;
    std::shared_ptr<FeedbackManager> feedback_;
    GenreDatabase genreDb_;
    GenreSpread genreSpread_; ///< VIZ-1, beside genre_targets.json
    SystemBrainDatabase systemBrainDb_;
    // Holds references to genreDb_ and feedback_; declared after them so it
    // is destroyed first.
    std::unique_ptr<AnalysisController> controller_;
    AnalysisResultModel analysisModel_;

    std::map<void*, std::function<void(const std::string&, int, int)>> pipelineProgressListeners_;
    std::map<void*, std::function<void()>> serviceRecoveredListeners_;
    std::map<void*, std::function<void()>> verificationListeners_;

    // FEAT-1: a second controller re-analyses accepted renders, so a check
    // never disturbs the published analysis or its caches. Checks are kept
    // per render path for this session's analysis (undo/redo reuse them).
    std::unique_ptr<AnalysisController> verifyController_;
    std::map<juce::String, fix_verification::Result> verifications_;
    juce::String verifiedPath_;  ///< the version whose check is current ("" = none)
    juce::String verifyingPath_; ///< the render being re-analysed ("" = idle)

    /// beginAnalysis() without resetting the recovery allowance; `inPlace`
    /// (FN2-2b) keeps the gate open and the current result on screen.
    void startAnalysis(bool forceRerun, bool inPlace = false);
    // FIX-2 / FN2-2b: what service recovery has already repeated since the
    // last beginAnalysis() - the analysis itself (how often, and at which
    // engine-up edge the last time), and each render's fix check.
    int recoveryReruns_ = 0;
    int engineUpEpoch_ = 0; ///< bumped on every not-running -> Running edge
    int rerunEpoch_ = -1;   ///< engineUpEpoch_ at the last recovery re-run
    bool engineRunning_ = false;
    bool upgradeInFlight_ = false; ///< the running analysis is an in-place re-run
    /// Re-runs once when the result records engine inputs that went
    /// unanswered; true when it started a run.
    bool rerunIfEngineInputsUnanswered();
    std::shared_ptr<service_ready::EngineGate> engineGate_ = std::make_shared<service_ready::EngineGate>();
    std::string scoreWeightProfile_; ///< OVH-1: "" = the session genre's profile
    bool deepFailedThisRun_ = false; ///< the engine answered "deep job failed"
    bool deepSkippedByUser_ = false; ///< cancelled after the fast pass
    bool provisionalShownThisRun_ = false;
    std::set<juce::String> verificationRetried_;

    bool analysisInFlight_ = false; // true between beginAnalysis() and its final/failed result
    bool skipPipelineForTesting_ = false;
    ResultsGate resultsGate_ = ResultsGate::Open;
    int followUpToken_ = 0;
    int analysisRunToken_ = 0;
    int followUpPending_ = 0;
    int followUpTotal_ = 0;

    JUCE_DECLARE_WEAK_REFERENCEABLE(AnalysisService)
    JUCE_DECLARE_NON_COPYABLE(AnalysisService)
};

} // namespace khensu
