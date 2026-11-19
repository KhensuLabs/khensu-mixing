// desktop-app/Source/analysis/DeepAnalysisJobRunner.cpp
#include "DeepAnalysisJobRunner.h"
#include <khensu/ipc/generated/ApiJobStatus.h>
#include <juce_core/juce_core.h>

namespace khensu {

DeepAnalysisJobResult runDeepAnalysisJob(ipc::ILocalServiceBridge& bridge, const std::string& filePath,
                                         const DeepAnalysisJobHooks& hooksIn, const DeepAnalysisPollConfig& cfg)
{
    // Fill in production defaults for any injectable hook the caller left unset.
    DeepAnalysisJobHooks hooks = hooksIn;
    if (!hooks.now) hooks.now = [] { return juce::Time::getMillisecondCounter(); };
    if (!hooks.sleep) hooks.sleep = [](int ms) { juce::Thread::sleep(ms); };

    DeepAnalysisJobResult result;

    auto jobStart = bridge.startAnalysisJob(filePath, hooks.mode);
    if (!jobStart.is_ok()) {
        result.outcome = DeepAnalysisOutcome::JobStartFailed;
        // Down or shedding load is "ask again later"; a 4xx is an answer.
        result.engineDidNotAnswer = ipc::engineDidNotAnswer(jobStart.error());
        return result;
    }
    const std::string jobId = jobStart.value();
    hooks.onJobId(jobId);

    ipc::AnalysisJobStatus jobStatus;
    const unsigned int startMs = hooks.now();
    int consecutiveErrors = 0;
    bool timedOutOrErrored = false;
    bool cancelledLocally = false;
    bool failedServerSide = false;
    bool interrupted = false; // the service shut down under the job

    while (true) {
        if (hooks.shouldCancel()) {
            cancelledLocally = true;
            break;
        }
        // Wall-clock deadline, not an iteration count: a stalled HTTP read can
        // burn seconds per poll, so counting iterations alone let a handful of
        // slow polls blow past the intended ceiling (CQ-04).
        if (hooks.now() - startMs > static_cast<unsigned int>(cfg.deadlineMs)) {
            timedOutOrErrored = true;
            break;
        }
        hooks.sleep(cfg.pollIntervalMs);

        auto poll = bridge.getAnalysisProgress(jobId);
        if (!poll.is_ok()) {
            // Consecutive poll failures are budgeted separately from the
            // deadline so a service outage mid-analysis doesn't spin for the
            // full deadline before giving up.
            if (++consecutiveErrors >= cfg.maxConsecutiveErrors) {
                timedOutOrErrored = true;
                break;
            }
            continue;
        }
        consecutiveErrors = 0;
        jobStatus = poll.value();
        if (khensu::api::isActiveStatus(jobStatus.status))
            hooks.onProgress(jobStatus.stage_index, jobStatus.total_stages);
        // "interrupted" = the service shut down under the job (common/jobs.py).
        if (jobStatus.status == khensu::api::job_status::failed ||
            jobStatus.status == khensu::api::job_status::interrupted) {
            failedServerSide = true;
            interrupted = jobStatus.status == khensu::api::job_status::interrupted;
            break;
        }
        if (khensu::api::isTerminalStatus(jobStatus.status)) break; // completed / cancelled
    }

    // An external cancel (shouldCancel) means the canceller already (or will)
    // POST /cancel via onJobId's recorded id. A local timeout/error-budget
    // exhaustion is this runner's own decision, so it must request server-side
    // cancellation itself rather than abandoning a still-running job.
    if (timedOutOrErrored && !cancelledLocally) bridge.cancelAnalysisJob(jobId);

    if (cancelledLocally) {
        result.outcome = DeepAnalysisOutcome::Cancelled;
        return result;
    }
    if (failedServerSide) {
        result.outcome = DeepAnalysisOutcome::FailedServerSide;
        result.engineDidNotAnswer = interrupted; // "failed" is an answer about the file
        result.errorMessage = jobStatus.error;
        return result;
    }
    if (timedOutOrErrored) {
        result.outcome = DeepAnalysisOutcome::TimedOutOrError;
        result.engineDidNotAnswer = true;
        return result;
    }
    if (jobStatus.status == khensu::api::job_status::cancelled) {
        // Cancelled on the service's side: no result is coming.
        result.outcome = DeepAnalysisOutcome::Cancelled;
        return result;
    }

    if (jobStatus.result.has_value()) {
        // The finished job already carries its parsed result: use it rather
        // than re-POSTing /analyze (which could re-run the whole pass on a
        // cache miss).
        result.hasResponse = true;
        result.response = std::move(*jobStatus.result);
    } else if (jobStatus.has_result) {
        // Older service / unparseable inline result: the blocking call parses
        // the identical dict (and is an instant cache hit when it works).
        if (auto deep = bridge.analyze(filePath, ipc::ILocalServiceBridge::AnalysisMode::Advanced); deep.is_ok()) {
            result.hasResponse = true;
            result.response = std::move(deep.value());
        } else {
            result.engineDidNotAnswer = ipc::engineDidNotAnswer(deep.error());
        }
    }
    result.outcome = DeepAnalysisOutcome::Completed;
    return result;
}

} // namespace khensu
