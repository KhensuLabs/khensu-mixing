// desktop-app/Source/session/BackgroundRunner.h
#pragma once
#include <JuceHeader.h>
#include <atomic>
#include <cstdint>
#include <functional>
#include "../ui/shared/SessionEpoch.h"

namespace khensu {

/// The desktop app's owned worker pools plus the session epoch (ENG-1, first
/// extraction from AppController). Extracted so the pool/epoch/shutdown
/// contract can be tested on its own and later handed to panels that need
/// nothing else from the controller.
///
/// Contract (see CLAUDE.md "Desktop concurrency"):
///  - run()      short panel fetches (8 workers)
///  - runLong()  render/agent polling loops, chat streams (6 workers), so they
///               can't starve short fetches
///  - runPoll()  health / playhead / event polls (2 workers), so a burst of
///               fetches never freezes the status or meters
///  - runRestart() the service restart only (1 worker, never queued behind
///               long jobs, DSK-10); dropped while one is in flight
///  - Every run()/runLong() job is tagged with the epoch it was queued under
///    (session_epoch::ScopedTaskEpoch), so its message-thread callback can drop
///    a result for a superseded session.
///  - shutdown() raises isShuttingDown() FIRST (so long jobs can cancel), then
///    drains every pool: jobs still QUEUED are dropped without running
///    (juce::ThreadPool::removeAllJobs), running ones are waited for up to the
///    per-pool timeout. Idempotent; the destructor calls it too.
class BackgroundRunner {
  public:
    BackgroundRunner() = default;
    ~BackgroundRunner() { shutdown(); }

    BackgroundRunner(const BackgroundRunner&) = delete;
    BackgroundRunner& operator=(const BackgroundRunner&) = delete;

    void run(std::function<void()> job) { backgroundPool_.addJob(tagged(std::move(job))); }
    void runLong(std::function<void()> job) { longJobPool_.addJob(tagged(std::move(job))); }
    void runPoll(std::function<void()> job) { pollPool_.addJob(std::move(job)); }

    /// Returns false (and drops `job`) while a restart is still in flight.
    bool runRestart(std::function<void()> job)
    {
        if (restartPool_.getNumJobs() > 0) return false;
        restartPool_.addJob(std::move(job));
        return true;
    }

    uint64_t sessionEpoch() const noexcept { return sessionEpoch_.load(std::memory_order_acquire); }
    /// Called whenever the active session changes (start / load / delete /
    /// file relink).
    void advanceEpoch() noexcept { sessionEpoch_.fetch_add(1, std::memory_order_acq_rel); }

    bool isShuttingDown() const noexcept { return shuttingDown_.load(std::memory_order_acquire); }

    void shutdown()
    {
        // Signal long jobs (hosted-chain renders) to cancel, THEN drain, so the
        // drain can't hang on a render whose panel would only cancel it later.
        shuttingDown_.store(true, std::memory_order_release);
        backgroundPool_.removeAllJobs(true, 5000);
        longJobPool_.removeAllJobs(true, 5000);
        pollPool_.removeAllJobs(true, 2000);
        // A restart in progress (graceful /shutdown + waits) is let finish.
        restartPool_.removeAllJobs(true, 15000);
    }

  private:
    std::function<void()> tagged(std::function<void()> job) const
    {
        return [epoch = sessionEpoch(), job = std::move(job)] {
            const session_epoch::ScopedTaskEpoch scope(epoch);
            job();
        };
    }

    std::atomic<bool> shuttingDown_{false};
    std::atomic<uint64_t> sessionEpoch_{0};
    juce::ThreadPool backgroundPool_{8};
    juce::ThreadPool longJobPool_{6};
    juce::ThreadPool pollPool_{2};
    juce::ThreadPool restartPool_{1};
};

} // namespace khensu
