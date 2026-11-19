// tests/ui/BackgroundRunnerTest.cpp
// Pins the contract of BackgroundRunner (ENG-1, extracted from AppController):
// epoch tagging, shutdown order and the single-flight restart pool.
#include <catch2/catch_test_macros.hpp>
#include "session/BackgroundRunner.h"

#include <optional>

using khensu::BackgroundRunner;

namespace {
// shutdown() drops jobs that have not started yet (juce::ThreadPool::
// removeAllJobs), so tests wait for the work they check before draining.
template <typename Pred> bool waitFor(Pred pred, int timeoutMs = 10000)
{
    const auto start = juce::Time::getMillisecondCounter();
    while (!pred()) {
        if (juce::Time::getMillisecondCounter() - start > static_cast<juce::uint32>(timeoutMs)) return false;
        juce::Thread::sleep(1);
    }
    return true;
}
} // namespace

TEST_CASE("BackgroundRunner tags jobs with the epoch they were queued under", "[split][runner]")
{
    BackgroundRunner runner;
    std::atomic<int> done{0};
    std::optional<uint64_t> seenShort, seenLong;
    std::atomic<bool> pollHadEpoch{true};

    runner.advanceEpoch();
    const auto queuedUnder = runner.sessionEpoch();
    runner.run([&] {
        seenShort = khensu::session_epoch::current();
        ++done;
    });
    runner.runLong([&] {
        seenLong = khensu::session_epoch::current();
        ++done;
    });
    runner.runPoll([&] {
        pollHadEpoch = khensu::session_epoch::current().has_value();
        ++done;
    });
    REQUIRE(waitFor([&] { return done.load() == 3; }));
    runner.shutdown();

    REQUIRE(seenShort.has_value());
    REQUIRE(seenLong.has_value());
    CHECK(*seenShort == queuedUnder);
    CHECK(*seenLong == queuedUnder);
    CHECK_FALSE(pollHadEpoch.load()); // polls are not session work
}

TEST_CASE("BackgroundRunner: a job queued before a session change is stale after it", "[split][runner]")
{
    BackgroundRunner runner;
    std::atomic<bool> release{false};
    std::atomic<bool> finished{false};
    std::optional<uint64_t> taskEpoch;
    runner.run([&] {
        while (!release.load())
            juce::Thread::sleep(1);
        taskEpoch = khensu::session_epoch::current();
        finished = true;
    });
    runner.advanceEpoch(); // session switched while the job was queued/running
    release = true;
    REQUIRE(waitFor([&] { return finished.load(); }));
    runner.shutdown();

    REQUIRE(taskEpoch.has_value());
    CHECK_FALSE(khensu::session_epoch::stillCurrent(taskEpoch, runner));
    CHECK(khensu::session_epoch::stillCurrent(std::nullopt, runner)); // unknown origin counts as current
}

TEST_CASE("BackgroundRunner raises isShuttingDown before draining", "[split][runner]")
{
    BackgroundRunner runner;
    std::atomic<bool> sawShutdown{false};
    // A long job that only finishes once it sees the flag: if shutdown()
    // drained first, this would hang until the drain timeout.
    runner.runLong([&] {
        const auto start = juce::Time::getMillisecondCounter();
        while (!runner.isShuttingDown() && juce::Time::getMillisecondCounter() - start < 10000)
            juce::Thread::sleep(1);
        sawShutdown = runner.isShuttingDown();
    });
    juce::Thread::sleep(20);
    CHECK_FALSE(runner.isShuttingDown());
    runner.shutdown();
    CHECK(sawShutdown.load());
    runner.shutdown(); // idempotent
}

TEST_CASE("BackgroundRunner drops a restart while one is in flight", "[split][runner]")
{
    BackgroundRunner runner;
    std::atomic<bool> release{false};
    std::atomic<int> restarts{0};
    CHECK(runner.runRestart([&] {
        while (!release.load())
            juce::Thread::sleep(1);
        ++restarts;
    }));
    CHECK_FALSE(runner.runRestart([&] { ++restarts; }));
    release = true;
    REQUIRE(waitFor([&] { return restarts.load() == 1; }));
    runner.shutdown();
    CHECK(restarts.load() == 1);
}
