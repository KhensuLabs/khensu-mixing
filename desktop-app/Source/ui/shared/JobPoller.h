// desktop-app/Source/ui/shared/JobPoller.h
// One job-poll loop for every desktop caller of a service job route
// (audit 2026-11-06 streamlining S5 / DSK-10 / WIRE-02 / UIX-07).
//
// Every long service job (stem separation, full intake, render, agent run,
// ...) is driven the same way: POST a start route, poll a status route until
// the job reaches a terminal state, cancel it server-side when the caller
// gives up. The hand-rolled loops this replaces each got a different part of
// that wrong - one ignored errors for 30 minutes, one gave up after 30 s and
// re-ran the whole job synchronously, others never cancelled. JobPoller
// fixes the policy once:
//
//   * bounded CONSECUTIVE errors (a crashed service ends the wait with
//     Outcome::ServiceLost instead of spinning until a wall-clock ceiling);
//   * a 404 on the status route is terminal (Outcome::NotFound) - the service
//     restarted and forgot the job, polling it again can never succeed;
//   * a DURATION-based deadline (not an iteration count, which drifts with
//     the per-request latency) - on expiry the job is cancelled server-side
//     (Outcome::TimedOut) so it stops consuming a worker;
//   * a caller stop check (session changed, panel destroyed, app shutting
//     down - backgroundShouldStop() is always consulted) that also cancels
//     the job server-side (Outcome::Stopped).
//
// Usage - on a background worker (runLongInBackground), never the message
// thread, since run() blocks for the job's lifetime:
//
//   JobPoller<ipc::SeparationJobStatus>::Hooks hooks;
//   hooks.start  = [&] { return bridge.startSeparationJob(file, dir, method); };
//   hooks.poll   = [&](const std::string& id) { return bridge.getSeparationProgress(id); };
//   hooks.cancel = [&](const std::string& id) { bridge.cancelSeparationJob(id); };
//   hooks.isTerminal = [](const ipc::SeparationJobStatus& s) { return api::isTerminalStatus(s.status); };
//   hooks.onProgress = [&](const ipc::SeparationJobStatus& s) { /* callAsync a progress update */ };
//   hooks.shouldStop = [gen, &counter] { return counter.load() != gen; }; // session changed
//   JobPoller<ipc::SeparationJobStatus>::Options opts;
//   opts.deadlineSeconds = 30 * 60;
//   const auto r = JobPoller<ipc::SeparationJobStatus>::run(hooks, opts);
//   if (r.outcome == JobPollOutcome::Finished) { /* r.status is the terminal status */ }
//   else { /* r.describe() is a user-facing line ("The analysis service stopped responding ...") */ }
//
// Callers that already have a job id (started elsewhere) leave `start` empty
// and pass the id to run(hooks, opts, existingJobId).
//
// The clock and the sleep are injectable (Options::now / Options::sleep) so
// the policy is unit-tested without real waiting (tests/ui/JobPollerTest.cpp).
// THREAD_CONTEXT(WorkerThread)
#pragma once
#include <JuceHeader.h>
#include <khensu/common/Result.h>
#include <khensu/ipc/ILocalServiceBridge.h> // ipc::BridgeError
#include "BackgroundStop.h"
#include <functional>
#include <optional>
#include <string>

namespace khensu {

enum class JobPollOutcome {
    Finished,    ///< the job reached a terminal status (completed / failed / cancelled)
    StartFailed, ///< the start route failed; nothing is running server-side
    NotFound,    ///< the status route returned 404: the service no longer knows the job
    ServiceLost, ///< too many consecutive poll errors (service crashed / unreachable)
    TimedOut,    ///< the duration deadline passed; the job was cancelled server-side
    Stopped      ///< the caller's stop check fired (or the app is shutting down); cancelled
};

template <typename Status> class JobPoller final {
  public:
    using PollResult = Result<Status, ipc::BridgeError>;
    using StartResult = Result<std::string, ipc::BridgeError>;

    struct Hooks {
        /// Starts the job and returns its id. Optional when run() is given an id.
        std::function<StartResult()> start;
        /// Fetches the job's current status.
        std::function<PollResult(const std::string& jobId)> poll;
        /// Best-effort server-side cancel. Called on TimedOut / Stopped.
        std::function<void(const std::string& jobId)> cancel;
        /// True once `status` is terminal (completed / failed / cancelled).
        std::function<bool(const Status&)> isTerminal;
        /// Optional: every successfully polled status (worker thread).
        std::function<void(const Status&)> onProgress;
        /// Optional: the caller no longer wants the result (session changed,
        /// owner destroyed). backgroundShouldStop() is always checked too.
        std::function<bool()> shouldStop;
    };

    struct Options {
        int pollIntervalMs = 500;
        /// Consecutive failed polls before giving up with ServiceLost. A
        /// successful poll resets the count, so a single hiccup never ends a
        /// long job.
        int maxConsecutiveErrors = 8;
        /// Wall-clock budget for the whole wait, in seconds.
        double deadlineSeconds = 600.0;
        /// Injectable for tests: monotonic milliseconds, and an interruptible
        /// sleep that returns false when the wait must stop.
        std::function<double()> now;
        std::function<bool(int ms)> sleep;
    };

    struct Outcome {
        JobPollOutcome outcome = JobPollOutcome::StartFailed;
        std::string jobId;
        std::optional<Status> status;   ///< last successfully polled status
        ipc::BridgeError lastError;     ///< the error that ended the wait, if any
        int consecutiveErrors = 0;

        bool finished() const noexcept { return outcome == JobPollOutcome::Finished; }

        /// One user-facing line for a wait that did not finish. ASCII; the
        /// caller wraps it into its own message.
        juce::String describe() const
        {
            switch (outcome) {
            case JobPollOutcome::Finished:
                return TRANS("Finished.");
            case JobPollOutcome::StartFailed:
                return TRANS("The job could not be started") +
                       (lastError.message.empty() ? juce::String(".") : ": " + juce::String(lastError.message));
            case JobPollOutcome::NotFound:
                return TRANS("The analysis service no longer knows this job (it probably restarted). Run it again.");
            case JobPollOutcome::ServiceLost:
                return TRANS("The analysis service stopped responding while the job was running (service lost).");
            case JobPollOutcome::TimedOut:
                return TRANS("The job took longer than allowed and was cancelled.");
            case JobPollOutcome::Stopped:
                return TRANS("The job was cancelled.");
            }
            return {};
        }
    };

    /// Blocks until the job ends or the wait is abandoned. See the header
    /// comment for the policy.
    static Outcome run(const Hooks& hooks, const Options& options, const std::string& existingJobId = {})
    {
        Outcome out;
        const auto now = options.now ? options.now
                                     : std::function<double()>([] { return juce::Time::getMillisecondCounterHiRes(); });
        const auto sleep = options.sleep ? options.sleep : std::function<bool(int)>([](int ms) {
            return backgroundSleep(ms);
        });
        const auto stopRequested = [&hooks] {
            return backgroundShouldStop() || (hooks.shouldStop && hooks.shouldStop());
        };
        const auto cancelRemote = [&hooks, &out] {
            if (hooks.cancel && !out.jobId.empty()) hooks.cancel(out.jobId);
        };

        if (!existingJobId.empty()) {
            out.jobId = existingJobId;
        } else {
            if (!hooks.start) {
                out.lastError = {ipc::BridgeError::Kind::ServiceUnavailable, "no start hook", 0};
                return out;
            }
            auto started = hooks.start();
            if (started.is_err()) {
                out.lastError = started.error();
                return out; // StartFailed
            }
            out.jobId = started.value();
        }

        const double deadline = now() + juce::jmax(0.0, options.deadlineSeconds) * 1000.0;
        for (;;) {
            if (stopRequested()) {
                cancelRemote();
                out.outcome = JobPollOutcome::Stopped;
                return out;
            }
            if (now() >= deadline) {
                cancelRemote();
                out.outcome = JobPollOutcome::TimedOut;
                return out;
            }
            if (!sleep(juce::jmax(1, options.pollIntervalMs))) {
                cancelRemote();
                out.outcome = JobPollOutcome::Stopped;
                return out;
            }
            if (stopRequested()) {
                cancelRemote();
                out.outcome = JobPollOutcome::Stopped;
                return out;
            }

            auto polled = hooks.poll(out.jobId);
            if (polled.is_err()) {
                out.lastError = polled.error();
                if (out.lastError.http_status == 404) {
                    out.outcome = JobPollOutcome::NotFound;
                    return out;
                }
                if (++out.consecutiveErrors >= juce::jmax(1, options.maxConsecutiveErrors)) {
                    // Best-effort: if the service is merely slow, free its worker.
                    cancelRemote();
                    out.outcome = JobPollOutcome::ServiceLost;
                    return out;
                }
                continue;
            }
            out.consecutiveErrors = 0;
            out.status = polled.value();
            if (hooks.onProgress) hooks.onProgress(*out.status);
            if (hooks.isTerminal && hooks.isTerminal(*out.status)) {
                out.outcome = JobPollOutcome::Finished;
                return out;
            }
        }
    }
};

} // namespace khensu
