// desktop-app/Source/analysis/AnalysisController.h
#pragma once
#include <JuceHeader.h>
#include <khensu/analysis/AnalysisResult.h>
#include <khensu/session/Session.h>
#include <khensu/recommendations/RecommendationEngine.h>
#include <khensu/ipc/DeepAnalysisResponse.h>
#include <khensu/analysis/ChordAnalysis.h>
#include <khensu/analysis/SectionAnalysis.h>
#include "DeepAnalysisJobRunner.h"
#include <functional>
#include <atomic>
#include <memory>
#include <optional>
#include <map>

namespace khensu {

class GenreDatabase;
class FeedbackManager;

/// The requests the pipeline makes of the analysis engine, as functions, so
/// a test can stand in for the engine (FIX-5). All are called on worker
/// threads; the five after `deep` run at the same time as each other.
struct AnalysisServiceCalls {
    using Error = ipc::BridgeError;
    std::function<bool()> available;
    std::function<DeepAnalysisJobResult(const std::string& path, const DeepAnalysisJobHooks& hooks)> deep;
    std::function<Result<SectionAnalysis, Error>(const std::string& path)> sections;
    std::function<Result<AnalysisResult::DrumData, Error>(const std::string& path)> drums;
    std::function<Result<ReferenceDeltas, Error>(const std::string& path, const std::string& reference)> reference;
    std::function<Result<ChordAnalysis, Error>(const std::string& path)> chords;
    std::function<Result<juce::var, Error>(const std::string& path)> trust;
    // `reference` and `chords` may be empty: the pipeline then skips them.

    /// Send sections / drums / trust with the deep job instead of after it.
    bool scansWithDeep = false;

    /// The local Python service, through HttpServiceBridge.
    static AnalysisServiceCalls localService();
    /// The fix check (FEAT-1): only what a finding is built from. The deep
    /// job leaves out the extras no finding reads (AnalysisMode::FixCheck),
    /// chords and the reference deltas are not asked for, and the other
    /// scans run alongside the deep job (docs/2026-11-14-fix-check-cost.md).
    static AnalysisServiceCalls fixCheck();
};

// THREAD_CONTEXT(UIThread for public API; runs work on an internal ThreadPool)
class AnalysisController {
  public:
    using ResultCallback = std::function<void(const AnalysisResult&)>;
    using ProgressCallback = std::function<void(const std::string& stage, int stageIndex, int totalStages)>;

    explicit AnalysisController(const GenreDatabase& genres, FeedbackManager* feedbackMgr,
                                AnalysisServiceCalls calls = AnalysisServiceCalls::localService());
    ~AnalysisController();

    /// `forceRerun` bypasses the same-session/unmodified-file result cache so a
    /// stale C++-only result (analyzed while the Python service was down) can be
    /// re-run once the service recovers, instead of re-delivering the cached one.
    void analyzeSession(const Session& session, ResultCallback onUpdate, ProgressCallback onProgress = nullptr,
                        bool forceRerun = false);
    void cancel();

    /// PERF-3: the stored native pass of `file`, when it can stand in for
    /// analysing the file again (same bytes, same native analysers); empty =
    /// analyse. Called on the worker, for a session that analyses one file.
    using StoredNativeLoader =
        std::function<std::optional<AnalysisResult>(const std::string& sessionId, const AudioFileRef& file)>;
    void setStoredNativeLoader(StoredNativeLoader loader)
    {
        storedNative_ = std::make_shared<const StoredNativeLoader>(std::move(loader));
    }

    /// Replaces the engine the next runs talk to. Tests only.
    void setServiceCallsForTesting(AnalysisServiceCalls calls)
    {
        service_ = std::make_shared<const AnalysisServiceCalls>(std::move(calls));
    }

    /// Session change (DSK-02): cancels any in-flight run (locally and
    /// server-side) and forgets the current result, so nothing of the
    /// previous session stays readable through currentResult(). A finished
    /// result with deep data is kept aside as the reopen cache: analysing the
    /// same unmodified session again re-delivers it without a re-run.
    void clear();

    /// Sets the reference track path. When non-empty, each analyzeSession() call
    /// will automatically call /analyze/reference and populate result.reference_deltas.
    void setReferenceFilePath(const std::string& path) { referenceFilePath_ = path; }
    bool hasReferencePath() const noexcept { return !referenceFilePath_.empty(); }
    const std::string& referenceFilePath() const noexcept { return referenceFilePath_; }

    bool hasResult() const noexcept { return hasResult_; }
    const AnalysisResult& currentResult() const noexcept { return current_; }

    /// Test seam (AnalysisService::deliverResultForTesting only): makes
    /// `result` the current result, as a finished run's delivery does,
    /// without running the pipeline. Not for production code.
    void adoptResultForTesting(const AnalysisResult& result)
    {
        current_ = result;
        hasResult_ = true;
    }

    /// Merges a masking analysis (run on-demand from StemMaskingPanel, not
    /// part of the main pipeline) into the current result so the next
    /// RecommendationEngine::generate() call includes MaskingRecommender's
    /// output. No-op if no analysis has run yet.
    void updateMasking(const MaskingSummary& masking)
    {
        if (hasResult_) current_.masking = masking;
    }

    /// WF-6: a new reference's comparison (or none) merged into the current
    /// result without re-running the rest of the pipeline.
    void updateReferenceDeltas(std::optional<ReferenceDeltas> deltas)
    {
        if (hasResult_) current_.reference_deltas = std::move(deltas);
    }

    /// Cache deep-analysis responses computed during Session Intake so that
    /// the first analyzeSession() for the same (unmodified) files reuses them
    /// instead of paying the 15-60 s Python pass a second time. Consumed once
    /// per matching file. THREAD_CONTEXT(UIThread)
    void cacheDeepResponse(const std::string& filePath, ipc::DeepAnalysisResponse response)
    {
        const juce::int64 mtime = juce::File(juce::String(filePath)).getLastModificationTime().toMilliseconds();
        preloadedDeep_[filePath] = {mtime, std::move(response)};
    }

  private:
    struct SharedState {
        std::mutex mutex;
        AnalysisController* owner = nullptr;
        std::atomic<int> generation{0};
        // The remote /analyze job currently in flight for `generation`, if
        // any — lets cancel() actually tell the service to stop the work
        // instead of only invalidating the local result (CQ-04).
        std::string currentJobId;
        int currentJobGeneration = -1;

        /// Whether run `gen` may still deliver: the controller is alive and
        /// no newer run or cancel() has superseded it. The lock is released
        /// before this returns, and that is the point (FIX-6): a delivery
        /// calls the listener WITHOUT the lock, because a listener may
        /// cancel or start the next run, and both take it. Deliveries, the
        /// destructor, cancel() and analyzeSession() all run on the message
        /// thread, so the answer cannot go stale between the check and the
        /// call.
        bool mayDeliver(int gen)
        {
            std::lock_guard<std::mutex> lock(mutex);
            return owner != nullptr && generation.load() == gen;
        }
    };
    std::shared_ptr<SharedState> sharedState_;

    const GenreDatabase& genres_;
    FeedbackManager* feedbackMgr_ = nullptr;
    AnalysisResult current_;
    bool hasResult_ = false;
    std::string cachedSessionId_;
    // Reopen cache: the last complete (deep) result, kept across clear().
    std::optional<AnalysisResult> reopenCache_;
    std::string referenceFilePath_;   // optional reference track for auto-comparison
    juce::int64 cachedFileMtime_ = 0; // modification time of cached file (ms)

    // Per-file deep-response cache from Session Intake (see cacheDeepResponse)
    struct PreloadedDeep {
        juce::int64 mtime = 0;
        ipc::DeepAnalysisResponse response;
    };
    std::map<std::string, PreloadedDeep> preloadedDeep_;
    /// Shared with each run's job, which may outlive a replacement.
    std::shared_ptr<const AnalysisServiceCalls> service_;
    std::shared_ptr<const StoredNativeLoader> storedNative_; ///< PERF-3, see setStoredNativeLoader
    juce::ThreadPool pool_{1};
    /// Server-side cancel POSTs: separate from pool_ so a cancel isn't queued
    /// behind the very analysis it is cancelling; owned, not detached.
    juce::ThreadPool cancelPool_{1};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AnalysisController)
};

} // namespace khensu
