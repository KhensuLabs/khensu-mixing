// desktop-app/Source/analysis/DeepAnalysisJobRunner.h
#pragma once
#include <khensu/ipc/ILocalServiceBridge.h>
#include <functional>
#include <string>

namespace khensu {

/// How a deep-analysis job-poll loop ended (audit CQ-04).
enum class DeepAnalysisOutcome {
    Completed,          ///< Job reached "completed"; a re-fetched response is attached.
    Cancelled,          ///< shouldCancel() went true (e.g. the session/generation changed).
    TimedOutOrError,    ///< Wall-clock deadline hit, or the consecutive-error budget was exhausted.
    JobStartFailed,     ///< startAnalysisJob() itself failed (service returned an error).
    FailedServerSide,   ///< The job reported status == "failed".
};

/// Injected dependencies so the runner is deterministically testable without
/// a live service, a real clock, or real sleeps (audit CQ-04's "injectable
/// bridge seam"). Every field has a production default; tests override them.
struct DeepAnalysisJobHooks {
    /// Polled between steps; returning true abandons the job (and POSTs a
    /// server-side cancel if one was started). This is how a session switch
    /// or a newer analysis request cancels an in-flight batch item.
    std::function<bool()> shouldCancel = [] { return false; };
    /// Reports the assigned job id as soon as it's known, so an external
    /// canceller (AnalysisController::cancel()) can also POST /cancel for it.
    std::function<void(const std::string& jobId)> onJobId = [](const std::string&) {};
    /// Reports live sub-progress within the deep-analysis stage.
    std::function<void(int stageIndex, int totalStages)> onProgress = [](int, int) {};
    /// Wall clock in ms (defaults to juce::Time::getMillisecondCounter); a
    /// test can jump this forward to exercise the deadline path with no real
    /// waiting.
    std::function<unsigned int()> now;
    /// Cooperative sleep between polls (defaults to juce::Thread::sleep); a
    /// test passes a no-op so the loop spins instantly.
    std::function<void(int ms)> sleep;
    /// Advanced, or FixCheck for checking a fixed version (FEAT-1).
    ipc::ILocalServiceBridge::AnalysisMode mode = ipc::ILocalServiceBridge::AnalysisMode::Advanced;
};

struct DeepAnalysisPollConfig {
    int deadlineMs = 120000;
    int maxConsecutiveErrors = 20;
    int pollIntervalMs = 100;
};

struct DeepAnalysisJobResult {
    DeepAnalysisOutcome outcome = DeepAnalysisOutcome::JobStartFailed;
    bool hasResponse = false;
    ipc::DeepAnalysisResponse response{}; ///< Valid iff hasResponse && outcome == Completed.
    /// True when there is no response because the engine did not answer: it
    /// was down or shedding load at the start, the polls timed out or kept
    /// failing, or it shut down under the job ("interrupted"). False when it
    /// refused or failed the request itself, and when the caller cancelled.
    /// Asking again later is only worth it when this is true (FIX-2).
    bool engineDidNotAnswer = false;
    /// The service's failure detail when outcome == FailedServerSide, when it sent one.
    std::string errorMessage;
};

/// Runs one file's deep-analysis job to completion (or cancellation/timeout)
/// with a wall-clock deadline, a consecutive-error budget, and cooperative
/// cancellation, then re-fetches the completed response. Previously only the
/// single-file path had this; the batch (>1 file) path used a single blocking
/// analyze() call with none of it (audit CQ-04). Extracted here so both paths
/// share it and it can be unit-tested against a fake ILocalServiceBridge.
///
/// On a local timeout/error (not an external cancel) the runner POSTs a
/// server-side cancel itself, so the service doesn't keep grinding on an
/// abandoned job.
///
/// THREAD_CONTEXT(WorkerThread) - blocking HTTP + sleeps; never call on the
/// audio or message thread.
DeepAnalysisJobResult runDeepAnalysisJob(ipc::ILocalServiceBridge& bridge, const std::string& filePath,
                                         const DeepAnalysisJobHooks& hooks, const DeepAnalysisPollConfig& cfg = {});

} // namespace khensu
