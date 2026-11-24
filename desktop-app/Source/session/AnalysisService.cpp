// desktop-app/Source/session/AnalysisService.cpp
// Moved from AppController.cpp / AppController_Playback.cpp /
// AppController_Session.cpp (ENG-1 stage C5); behaviour unchanged. What used
// to touch AppController's widgets directly now goes through the hooks.
#include <khensu/analysis/AnalysisInputs.h>
#include "AnalysisService.h"
#include "../ui/shared/ScanSummary.h"
#include "KeyedListeners.h"
#include "SessionService.h"
#include "../IBackgroundWork.h"
#include <khensu/ipc/HttpServiceBridge.h>
#include "../analysis/AnalysisController.h"
#include "../analysis/ResultDerivedData.h"
#include <khensu/feedback/FeedbackManager.h>
#include <khensu/recommendations/ScoreWeightProfile.h>
#include <khensu/targets/GenreConfirmation.h>

namespace khensu {

AnalysisService::AnalysisService(IBackgroundWork& work, SessionService& sessions) : work_(work), sessions_(sessions)
{
}

AnalysisService::~AnalysisService() = default;

void AnalysisService::loadReferenceData(const juce::File& genreFile, const juce::File& systemBrainFile)
{
    if (genreFile.existsAsFile()) genreDb_.loadFromFile(genreFile.getFullPathName().toStdString());
    if (const auto spread = genreFile.getSiblingFile("genre_spread.json"); spread.existsAsFile())
        genreSpread_ = GenreSpread::fromFile(spread.getFullPathName().toStdString());
    if (systemBrainFile.existsAsFile()) systemBrainDb_.loadFromFile(systemBrainFile.getFullPathName().toStdString());
}

void AnalysisService::attachFeedbackManager(std::shared_ptr<FeedbackManager> feedback)
{
    feedback_ = std::move(feedback);
    controller_ = std::make_unique<AnalysisController>(genreDb_, feedback_.get(),
                                                       AnalysisServiceCalls::localService(engineGate_));
    // PERF-3: a reopen reuses the stored native pass of an unchanged file.
    // The store is attached after this runs, so it is looked up per call
    // (sharedManager() is safe off the message thread; the service owning
    // `sessions_` outlives the controller).
    controller_->setStoredNativeLoader([&sessions = sessions_](const std::string& sessionId,
                                                               const AudioFileRef& file) -> std::optional<AnalysisResult> {
        const auto sm = sessions.sharedManager();
        if (sm == nullptr || !sm->isOpen()) return std::nullopt;
        auto loaded = sm->loadReusableNative(sessionId, file.id, file.path);
        return loaded.is_ok() ? loaded.value() : std::nullopt;
    });
    // The fix check asks only for what a finding is built from (FEAT-1).
    verifyController_ =
        std::make_unique<AnalysisController>(genreDb_, feedback_.get(), AnalysisServiceCalls::fixCheck(engineGate_));
}

void AnalysisService::recordScanResult(const juce::String& scan, const juce::var& result)
{
    const auto compact = scan_summary::compact(result);
    if (compact.isVoid()) return;
    if (scanResults_ == nullptr) scanResults_ = new juce::DynamicObject();
    scanResults_->setProperty(juce::Identifier(scan), compact);
}

juce::var AnalysisService::scanResults() const
{
    return scanResults_ != nullptr ? juce::var(scanResults_.get()) : juce::var();
}

bool AnalysisService::hasAnalysis() const noexcept { return controller_ && controller_->hasResult(); }

const AnalysisResult& AnalysisService::currentAnalysis() const noexcept
{
    jassert(controller_ != nullptr);
    return controller_->currentResult();
}

void AnalysisService::setStemMaskingResult(const MaskingSummary& masking)
{
    if (!hasAnalysis()) return;
    controller_->updateMasking(masking);
    analysisModel_.setResult(currentAnalysis());
}

const ResultDerivedData& AnalysisService::derivedResultData(const std::string& weightProfileId)
{
    GenreEntry genre;
    CreativeTarget target;
    std::string sessionId;
    if (sessions_.state().session) {
        target = sessions_.state().session->target;
        sessionId = sessions_.state().session->id;
        if (auto g = genreDb_.findById(target.primary_genre)) genre = *g;
    }
    const std::string& chosen = weightProfileId.empty() ? scoreWeightProfile_ : weightProfileId;
    const std::string profileId = chosen.empty() ? (genre.id.empty() ? std::string("default") : genre.id) : chosen;
    const auto key = ResultDerivedData::keyFor(sessionId, genre, target, profileId);
    auto* fm = feedback_.get();
    return analysisModel_.derived<ResultDerivedData>(key, [&](const AnalysisResult& r) {
        ScoreWeightProfile weights;
        if (fm != nullptr) weights.loadFromDatabase(*fm, profileId);
        return ResultDerivedData::compute(r, genre, target, weights);
    });
}

void AnalysisService::notifyPipelineProgress(const std::string& stage, int idx, int total)
{
    notifyKeyedListeners(pipelineProgressListeners_, stage, idx, total);
}

// ---- Running an analysis -------------------------------------------------------

void AnalysisService::reanalyzeIfServiceRecovered()
{
    // An unhealthy -> healthy edge by definition - unless setEngineStatus()
    // already counted it (the lifecycle reports Running just before this).
    if (!engineRunning_) ++engineUpEpoch_;
    // An in-flight analysis is left alone: it reaches the engine by itself.
    if (analysisInFlight_ || !sessions_.hasSession()) return;
    // Only a settled result on screen can gain from the engine coming back.
    // A cancelled run stays cancelled, a failed one (the file itself) is not
    // failed again, and "no deep half" alone is not a reason: deep analysis
    // may be switched off for this session (FIX-2).
    if (resultsGate_ != ResultsGate::Open || !hasAnalysis()) return;

    if (rerunIfEngineInputsUnanswered()) return;

    // The current version's fix check went unanswered (FIX-1): it says
    // "could not check" for what a complete check would answer. Once per render.
    if (verifyingPath_.isEmpty()) {
        const auto* check = currentVerification();
        if (check != nullptr && check->inputsFailed != 0 && verificationRetried_.insert(verifiedPath_).second) {
            const juce::String path = verifiedPath_; // verifyRender() clears verifiedPath_
            verifications_.erase(path);
            verifyRender(path);
        }
    }

    // Service-backed scans that ran while it was down still show
    // "unreachable" - let them re-run (DSK-03).
    notifyKeyedListeners(serviceRecoveredListeners_);
}

bool AnalysisService::rerunIfEngineInputsUnanswered()
{
    if (analysisInFlight_ || !sessions_.hasSession()) return false;
    if (resultsGate_ != ResultsGate::Open || !hasAnalysis()) return false;
    const auto unanswered = currentAnalysis().inputs_failed;
    if (unanswered == 0 || recoveryReruns_ >= kMaxRecoveryReruns) return false;
    // After the first, only when the engine came up again since the last
    // one: a busy engine that is still Running answers no better now.
    if (recoveryReruns_ > 0 && rerunEpoch_ == engineUpEpoch_) return false;
    juce::Logger::writeToLog("AnalysisService: engine available - re-running the analysis in place for "
                             + juce::String(analysis_inputs::describe(unanswered)));
    ++recoveryReruns_;
    rerunEpoch_ = engineUpEpoch_;
    startAnalysis(/*forceRerun*/ true, /*inPlace*/ true);
    return true;
}

void AnalysisService::setEngineStatus(PythonStatus status)
{
    using S = service_ready::EngineGate::State;
    switch (status) {
    case PythonStatus::Running: engineGate_->publish(S::Running); break;
    case PythonStatus::Starting: engineGate_->publish(S::Starting); break;
    case PythonStatus::Stopped:
    case PythonStatus::GaveUp: engineGate_->publish(S::Down); break;
    }
    const bool running = status == PythonStatus::Running;
    if (running && !engineRunning_) ++engineUpEpoch_;
    engineRunning_ = running;
    if (running) rerunIfEngineInputsUnanswered();
}

DeepPassState AnalysisService::deepPassState() const
{
    if (hasAnalysis() && currentAnalysis().has_python_analysis()) return DeepPassState::Complete;
    if (sessions_.state().session && !sessions_.state().session->target.run_deep_analysis)
        return DeepPassState::Off;
    if (deepSkippedByUser_) return DeepPassState::SkippedByUser;
    if (deepFailedThisRun_) return DeepPassState::Failed;
    if (hasAnalysis() && (currentAnalysis().inputs_failed & analysis_inputs::kDeep) != 0)
        return DeepPassState::EngineUnavailable;
    return DeepPassState::Failed;
}

void AnalysisService::reanalyzeActiveSession()
{
    if (sessions_.hasSession()) beginAnalysis(/*forceRerun*/ true);
}

void AnalysisService::beginAnalysis(bool forceRerun)
{
    // A run the user (or opening a session) asked for earns its automatic
    // repeats if the engine does not answer it, and one per fix check.
    recoveryReruns_ = 0;
    rerunEpoch_ = -1;
    verificationRetried_.clear();
    startAnalysis(forceRerun);
}

void AnalysisService::startAnalysis(bool forceRerun, bool inPlace)
{
    if (!sessions_.state().session) return;
    // A session with no file has nothing to analyse, and the pipeline would
    // return without ever reporting - leaving "Analyzing..." up for good
    // (FIX-6). Say so instead of starting a run.
    if (!skipPipelineForTesting_ && sessions_.state().session->files.empty()) {
        analysisInFlight_ = false;
        setResultsGate(ResultsGate::Failed);
        if (onFailed) onFailed(TRANS("This session has no audio file to analyze."));
        return;
    }
    analysisInFlight_ = true;
    upgradeInFlight_ = inPlace;
    deepFailedThisRun_ = false;
    deepSkippedByUser_ = false;
    provisionalShownThisRun_ = false;
    ++analysisRunToken_;
    if (inPlace) {
        // FN2-2b: the result on screen (the native pass, the engine's inputs
        // unanswered) stays there, gate open, until this run's final result
        // replaces it; the follow-up scans of that result keep reporting.
        if (onStatus) onStatus(TRANS("Deep analysis..."), true, /*inProgress*/ true);
    } else {
        // Withhold every finding and grade until the main analysis is done; a
        // new batch token makes late follow-up scans of a previous run harmless.
        // (The owner empties the health strip on Analyzing.)
        followUpPending_ = 0;
        followUpTotal_ = 0;
        ++followUpToken_;
        setResultsGate(ResultsGate::Analyzing);

        // Show immediate status before the first result arrives (fast analysis takes ~1-5s,
        // deep analysis can take 15-60s; without this the sidebar stays on "Pending").
        if (onStatus) onStatus(TRANS("Analyzing..."), true, /*inProgress*/ true);
    }

    const std::string sessionId = sessions_.state().session->id;
    const std::string fileId =
        sessions_.state().session->files.empty() ? std::string{} : sessions_.state().session->files.front().id;
    // PERF-3: the stored native pass is stamped with its file (so a reopen
    // can reuse it) only when the session analyses exactly that one file.
    std::string sourcePath;
    if (const auto analysed = sessions_.state().session->analysedFiles();
        analysed.size() == 1 && analysed.front().id == fileId)
        sourcePath = analysed.front().path;

    if (skipPipelineForTesting_) return;
    // `this` is safe in both callbacks: the controller only delivers while it
    // is alive (SharedState::owner), and this service owns it.
    controller_->analyzeSession(
        *sessions_.state().session,
        [this, sessionId, fileId, sourcePath](const AnalysisResult& r) { handleUpdate(sessionId, fileId, r, sourcePath); },
        [this](const std::string& stage, int idx, int total) { handleStage(stage, idx, total); }, forceRerun,
        /*keepCurrentResult*/ inPlace);
}

void AnalysisService::handleUpdate(const std::string& sessionId, const std::string& fileId, const AnalysisResult& r,
                                   const std::string& sourcePath)
{
    // DSK-02: a result for a session that is no longer open is dropped
    // (AnalysisController::clear() on the switch already invalidates
    // its generation; this is the belt to that brace).
    if (!sessions_.state().session || sessions_.state().session->id != sessionId) return;
    if (r.analysis_failed && upgradeInFlight_) {
        // FN2-2b: the in-place re-run failed; the result on screen stands.
        analysisInFlight_ = false;
        upgradeInFlight_ = false;
        juce::Logger::writeToLog("AnalysisService: the in-place re-run failed (" + juce::String(r.error_message)
                                 + ") - the result on screen stands");
        setResultsGate(ResultsGate::Open); // back to "Ready"
        return;
    }
    if (r.analysis_failed) {
        analysisInFlight_ = false;
        setResultsGate(ResultsGate::Failed);
        // The job hit an exception or every file failed to decode -
        // surface it instead of leaving the sidebar on "Analyzing..."
        // forever. Don't persist or notify listeners with empty data.
        if (onFailed) onFailed(juce::String(r.error_message));
        return;
    }
    if (r.is_partial) {
        // Fast C++ analysis complete; Python deep analysis is now
        // running. The partial is NOT published to the listeners: its
        // grades would change when the deep stages land (results gate).
        // Its plain measurements show at once, marked provisional (PERF-1).
        if (onStatus) onStatus(TRANS("Deep analysis..."), true, /*inProgress*/ true);
        provisionalShownThisRun_ = true;
        if (onProvisionalResult && resultsGate_ == ResultsGate::Analyzing) onProvisionalResult(r);
        return;
    }
    // Full analysis complete (FN2-2b: for an in-place re-run, the result
    // that supersedes the one on screen - published the same way).
    analysisInFlight_ = false;
    upgradeInFlight_ = false;
    // Final: the Dashboard's listener auto-starts its follow-up scans
    // inside this broadcast (addFollowUpScan). They don't hold the gate:
    // none of them changes a grade, so each panel fills in when its scan
    // lands (PERF-1). The gate opens right after the broadcast, so the top
    // strip (filled on Open) reads the new result.
    followUpPending_ = 0;
    followUpTotal_ = 0;
    // A new analysis of the original is a new baseline: earlier checks of
    // renders compared against the old one (FEAT-1). The owner asks for the
    // current version's check again.
    if (!verifications_.empty() || verifiedPath_.isNotEmpty()) {
        verifications_.clear();
        verifiedPath_.clear();
        notifyVerification();
    }
    // Cache + fire all registered analysis listeners (multicast).
    analysisModel_.setResult(r);
    setResultsGate(ResultsGate::Open);
    if (onFinalResult) onFinalResult();

    // Persist with its readiness. Readiness needs genre/target/weight
    // context SessionManager (pure persistence) doesn't have; it comes
    // from the shared per-result derived data (§10.3) - the
    // Dashboard's listener above usually computed it already, so this
    // is a cache hit rather than a second engine + report run.
    if (sessions_.sharedManager() && sessions_.manager().isOpen() && hasAnalysis()) {
        // OVH-1: the readiness saved is the one the Dashboard shows - the
        // scorecard's overall score under the chosen weighting.
        const float readiness = derivedResultData().scorecard.overall_score;
        work_.runInBackground([sm = sessions_.sharedManager(), r, sessionId, fileId, readiness, sourcePath]() {
            const auto saved = sm->saveAnalysisResult(r, sessionId, fileId, readiness, sourcePath);
            if (saved.is_err())
                juce::Logger::writeToLog("AppController: analysis completed but was not saved: " +
                                         juce::String(saved.error()));
        });
    }
}

void AnalysisService::handleStage(const std::string& stage, int idx, int total)
{
    // Feed the global HUD first (visible on every screen), the results
    // gate, then the Dashboard's detailed bar via the listeners.
    if (stage == "deep_analysis:failed") deepFailedThisRun_ = true;
    if (onStage) onStage(stage, idx, total);
    notifyPipelineProgress(stage, idx, total);
}

// ---- Results gate ----------------------------------------------------------------

void AnalysisService::setResultsGate(ResultsGate gate)
{
    resultsGate_ = gate;
    if (onGateChanged) onGateChanged(gate);
}

void AnalysisService::addFollowUpScan(int token)
{
    if (token != followUpToken_) return;
    ++followUpPending_;
    followUpTotal_ = juce::jmax(followUpTotal_, followUpPending_);
    if (onFollowUpsChanged) onFollowUpsChanged(followUpPending_, followUpTotal_);
}

void AnalysisService::followUpScanFinished(int token)
{
    if (token != followUpToken_ || followUpPending_ <= 0) return;
    followUpPending_ = juce::jmax(0, followUpPending_ - 1);
    if (onFollowUpsChanged) onFollowUpsChanged(followUpPending_, followUpTotal_);
}

void AnalysisService::cancelAnalysis()
{
    // Once results are shown there is nothing to cancel; a late click on the
    // Dashboard's progress bar must not hide finished results.
    if (resultsGate_ == ResultsGate::Open || resultsGate_ == ResultsGate::Cancelled) return;
    upgradeInFlight_ = false;
    if (controller_) controller_->cancel();
    deepSkippedByUser_ = provisionalShownThisRun_; // the fast pass was in; the deep one was not wanted
    analysisInFlight_ = false;
    ++followUpToken_; // drop any follow-up scans still reporting
    followUpPending_ = 0;
    if (onStatus) onStatus(TRANS("Cancelled"), false, false);
    if (onCancelled) onCancelled();
    setResultsGate(sessions_.hasSession() ? ResultsGate::Cancelled : ResultsGate::Open);
}

// ---- Fix -> verify (FEAT-1) --------------------------------------------------------

void AnalysisService::verifyRender(const juce::String& renderPath)
{
    const auto* session = sessions_.active();
    if (renderPath.isEmpty() || session == nullptr || session->files.empty() || !hasAnalysis()) {
        // The original is current (or there is nothing to compare with).
        if (verifyingPath_.isNotEmpty() && verifyController_) verifyController_->clear();
        const bool changed = verifiedPath_.isNotEmpty() || verifyingPath_.isNotEmpty();
        verifiedPath_.clear();
        verifyingPath_.clear();
        if (changed) notifyVerification();
        return;
    }
    if (verifications_.count(renderPath) != 0) { // checked already (undo / redo)
        if (verifyingPath_.isNotEmpty() && verifyController_) verifyController_->clear();
        verifyingPath_.clear();
        verifiedPath_ = renderPath;
        notifyVerification();
        return;
    }
    if (verifyingPath_ == renderPath || !verifyController_) return;
    if (verifyingPath_.isNotEmpty()) verifyController_->clear(); // a newer version supersedes it

    // The render analysed as this session's file: same id, target and
    // reference, so its findings carry the same issue codes as the original's.
    Session copy = *session;
    copy.files.resize(1);
    copy.files.front().path = renderPath.toStdString();
    verifyController_->setReferenceFilePath(controller_ ? controller_->referenceFilePath() : std::string{});
    verifyingPath_ = renderPath;
    verifiedPath_.clear();
    notifyVerification();
    if (onVerificationStatus) onVerificationStatus(TRANS("Checking the fix..."), true);
    verifyController_->analyzeSession(
        copy, [this, renderPath, sessionId = session->id](const AnalysisResult& r) {
            if (!sessions_.state().session || sessions_.state().session->id != sessionId) return;
            handleVerification(renderPath, r);
        });
}

void AnalysisService::handleVerification(juce::String renderPath, const AnalysisResult& r)
{
    if (r.is_partial || renderPath != verifyingPath_) return;
    verifyingPath_.clear();
    if (r.analysis_failed || !hasAnalysis()) {
        if (onVerificationStatus) onVerificationStatus(TRANS("Could not check the fix"), false);
        notifyVerification();
        return;
    }
    auto result = fix_verification::compare(currentAnalysis(), r, renderPath.toStdString());
    const juce::String summary(fix_verification::summary(result));
    verifications_[renderPath] = std::move(result);
    verifiedPath_ = renderPath;
    if (onVerificationStatus) onVerificationStatus(TRANS("Fix checked:") + " " + summary, false);
    notifyVerification();
}

const fix_verification::Result* AnalysisService::currentVerification() const noexcept
{
    if (verifiedPath_.isEmpty()) return nullptr;
    auto it = verifications_.find(verifiedPath_);
    return it == verifications_.end() ? nullptr : &it->second;
}

void AnalysisService::notifyVerification()
{
    // Copy first: a listener may deregister itself.
    const auto listeners = verificationListeners_;
    for (const auto& [key, cb] : listeners)
        if (cb && verificationListeners_.count(key) != 0) cb();
}

// ---- Reference track --------------------------------------------------------------

void AnalysisService::setReferenceTrack(const juce::String& path)
{
    if (controller_) controller_->setReferenceFilePath(path.toStdString());
    if (onReferenceChanged) onReferenceChanged();
    refreshReferenceComparison(path);
}

void AnalysisService::refreshReferenceComparison(const juce::String& path)
{
    // WF-6: a new reference re-runs only the reference comparison - the rest
    // of the analysis stands - and republishes, so every reader (the Compare
    // card, the recommendations, chat) sees the new reference at once.
    // Before, the deltas changed only on the next full analysis.
    const auto* session = sessions_.active();
    if (!hasAnalysis() || session == nullptr || session->files.empty()) return;
    if (path.isEmpty()) {
        controller_->updateReferenceDeltas(std::nullopt);
        analysisModel_.setResult(currentAnalysis());
        return;
    }
    const std::string target = session->files.front().path;
    const std::string reference = path.toStdString();
    const std::string sessionId = session->id;
    juce::WeakReference<AnalysisService> self(this);
    work_.runInBackground([self, target, reference, sessionId] {
        auto result = ipc::HttpServiceBridge{}.analyzeReference(target, reference);
        juce::MessageManager::callAsync([self, reference, sessionId, result = std::move(result)]() mutable {
            auto* s = self.get();
            if (s == nullptr) return;
            // Another session opened meanwhile: this comparison is not its.
            if (s->sessions_.active() == nullptr || s->sessions_.active()->id != sessionId) return;
            // The user may have picked another reference meanwhile.
            if (s->getReferenceTrack().toStdString() != reference || !s->hasAnalysis()) return;
            if (result.is_err()) return; // the Compare card reports its own errors
            s->controller_->updateReferenceDeltas(result.value());
            s->analysisModel_.setResult(s->currentAnalysis());
        });
    });
}

juce::String AnalysisService::getReferenceTrack() const
{
    return controller_ ? juce::String(controller_->referenceFilePath()) : juce::String();
}

bool AnalysisService::genreRelativeActionAllowed() const
{
    const auto* session = sessions_.active();
    if (session == nullptr || !hasAnalysis()) return false;
    return genre_confirmation::actionAllowed(session->target, getReferenceTrack().isNotEmpty(),
                                             currentAnalysis().genre_is_hybrid);
}

juce::String AnalysisService::genreRelativeAbstainedReason() const
{
    const auto* session = sessions_.active();
    if (session == nullptr) return TRANS("Open a session first.");
    if (!hasAnalysis()) return TRANS("Analyze the session first.");
    if (genreRelativeActionAllowed()) return {};
    if (!session->target.use_genre_targets)
        return TRANS("Genre-relative targets are turned off for this session.");
    if (currentAnalysis().genre_is_hybrid)
        return TRANS("Sections disagree on the genre - confirm a genre or load a reference track.");
    return TRANS("Confirm the genre (Song Info card) or load a reference track first.");
}

void AnalysisService::cacheIntakeAnalysis(const juce::String& path, ipc::DeepAnalysisResponse response)
{
    if (controller_) controller_->cacheDeepResponse(path.toStdString(), std::move(response));
}

// ---- Session change ------------------------------------------------------------------

void AnalysisService::resetForSession()
{
    // The reference track is per session: it was carried into the next one
    // and autosaved there (AUDIT-039). Cleared directly, not through
    // setReferenceTrack(), whose autosave would still target the OLD session;
    // the owner's restoreAutosavedState() brings back the new session's own.
    if (controller_) controller_->setReferenceFilePath({});

    // DSK-02: the previous session's analysis and gate state must not stay
    // live under the next session's name. Cancel the in-flight analysis
    // (locally and server-side) and forget its result; the gate shows "no
    // analysis" until the next session's own analysis runs.
    if (controller_) controller_->clear();
    analysisModel_.clear();
    scanResults_ = nullptr; // WF-14: the previous session's follow-up scans
    analysisInFlight_ = false;
    if (verifyController_) verifyController_->clear();
    verifications_.clear();
    verifiedPath_.clear();
    verifyingPath_.clear();
    recoveryReruns_ = 0;
    rerunEpoch_ = -1;
    upgradeInFlight_ = false;
    verificationRetried_.clear();
    ++analysisRunToken_;
    ++followUpToken_; // follow-up scans of the old session report into nothing
    followUpPending_ = 0;
    followUpTotal_ = 0;
    resultsGate_ = ResultsGate::Open; // set properly by beginAnalysis() / the missing-file path
}

// ---- Test seam ------------------------------------------------------------------------

void AnalysisService::setServiceCallsForTesting(const AnalysisServiceCalls& calls)
{
    if (controller_) controller_->setServiceCallsForTesting(calls);
}

void AnalysisService::deliverResultForTesting(const AnalysisResult& r, const std::string& forSessionId)
{
    if (!sessions_.state().session) return;
    const auto& session = *sessions_.state().session;
    const std::string sessionId = forSessionId.empty() ? session.id : forSessionId;
    const std::string fileId = session.files.empty() ? std::string{} : session.files.front().id;
    // The controller never adopts a stale run's result (its generation was
    // invalidated by the switch), so neither does the seam.
    if (controller_ && !r.is_partial && !r.analysis_failed && sessionId == session.id) {
        controller_->adoptResultForTesting(r);
        handleUpdate(sessionId, fileId, controller_->currentResult());
        return;
    }
    handleUpdate(sessionId, fileId, r);
}

void AnalysisService::deliverVerificationForTesting(const AnalysisResult& renderResult)
{
    handleVerification(verifyingPath_, renderResult);
}

void AnalysisService::deliverStageForTesting(const std::string& stage, int idx, int total)
{
    handleStage(stage, idx, total);
}

} // namespace khensu
