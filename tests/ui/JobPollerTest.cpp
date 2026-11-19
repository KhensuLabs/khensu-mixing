// tests/ui/JobPollerTest.cpp
// The shared job-poll policy (audit 2026-11-06 S5 / DSK-10 / WIRE-02): bounded
// consecutive errors, 404 terminal, duration deadline with server cancel,
// caller stop with server cancel. Clock and sleep are injected - no waiting.
#include <catch2/catch_test_macros.hpp>
#include "ui/shared/JobPoller.h"
#include <string>
#include <vector>

using namespace khensu;

namespace {
struct FakeStatus {
    std::string state;
};
using Poller = JobPoller<FakeStatus>;
using PollResult = Poller::PollResult;

struct Harness {
    double nowMs = 0.0;
    int polls = 0;
    std::vector<std::string> cancelled;
    Poller::Options options()
    {
        Poller::Options o;
        o.pollIntervalMs = 500;
        o.maxConsecutiveErrors = 3;
        o.deadlineSeconds = 10.0;
        o.now = [this] { return nowMs; };
        o.sleep = [this](int ms) {
            nowMs += ms;
            return true;
        };
        return o;
    }
    Poller::Hooks hooks(std::function<PollResult()> next)
    {
        Poller::Hooks h;
        h.start = [] { return Poller::StartResult::ok("job-1"); };
        h.poll = [this, next](const std::string&) {
            ++polls;
            return next();
        };
        h.cancel = [this](const std::string& id) { cancelled.push_back(id); };
        h.isTerminal = [](const FakeStatus& s) { return s.state == "completed" || s.state == "failed"; };
        return h;
    }
};

PollResult status(const char* s) { return PollResult::ok(FakeStatus{s}); }
PollResult error(int http = 0)
{
    return PollResult::err({ipc::BridgeError::Kind::ServiceUnavailable, "down", http});
}
} // namespace

TEST_CASE("JobPoller: finishes on a terminal status and reports progress", "[jobpoller]")
{
    Harness h;
    int n = 0;
    auto hooks = h.hooks([&] { return ++n < 3 ? status("running") : status("completed"); });
    int progress = 0;
    hooks.onProgress = [&](const FakeStatus&) { ++progress; };
    const auto r = Poller::run(hooks, h.options());
    CHECK(r.outcome == JobPollOutcome::Finished);
    CHECK(r.status->state == "completed");
    CHECK(progress == 3);
    CHECK(h.cancelled.empty());
}

TEST_CASE("JobPoller: consecutive errors end the wait as service-lost (and cancel)", "[jobpoller]")
{
    Harness h;
    const auto r = Poller::run(h.hooks([] { return error(); }), h.options());
    CHECK(r.outcome == JobPollOutcome::ServiceLost);
    CHECK(h.polls == 3); // maxConsecutiveErrors, not a 30-minute ceiling
    CHECK(h.cancelled == std::vector<std::string>{"job-1"});
    CHECK(r.describe().containsIgnoreCase("service lost"));
}

TEST_CASE("JobPoller: a successful poll resets the error budget", "[jobpoller]")
{
    Harness h;
    int n = 0;
    // error, error, ok, error, error, ok, completed - never 3 in a row.
    const char* script[] = {"e", "e", "running", "e", "e", "running", "completed"};
    const auto r = Poller::run(h.hooks([&] {
                                   const std::string s = script[n++];
                                   return s == "e" ? error() : status(s.c_str());
                               }),
                               h.options());
    CHECK(r.outcome == JobPollOutcome::Finished);
}

TEST_CASE("JobPoller: a 404 is terminal immediately", "[jobpoller]")
{
    Harness h;
    const auto r = Poller::run(h.hooks([] { return error(404); }), h.options());
    CHECK(r.outcome == JobPollOutcome::NotFound);
    CHECK(h.polls == 1);
}

TEST_CASE("JobPoller: the duration deadline cancels the job server-side", "[jobpoller]")
{
    Harness h;
    const auto r = Poller::run(h.hooks([] { return status("running"); }), h.options());
    CHECK(r.outcome == JobPollOutcome::TimedOut);
    CHECK(h.nowMs >= 10000.0);
    CHECK(h.polls == 20); // 10 s / 500 ms
    CHECK(h.cancelled == std::vector<std::string>{"job-1"});
}

TEST_CASE("JobPoller: the caller's stop check cancels the job", "[jobpoller]")
{
    Harness h;
    int n = 0;
    auto hooks = h.hooks([&] {
        ++n;
        return status("running");
    });
    hooks.shouldStop = [&] { return n >= 2; }; // e.g. the session changed
    const auto r = Poller::run(hooks, h.options());
    CHECK(r.outcome == JobPollOutcome::Stopped);
    CHECK(h.cancelled == std::vector<std::string>{"job-1"});
}

TEST_CASE("JobPoller: a failed start is reported and nothing is cancelled", "[jobpoller]")
{
    Harness h;
    auto hooks = h.hooks([] { return status("running"); });
    hooks.start = [] {
        return Poller::StartResult::err({ipc::BridgeError::Kind::HttpError, "boom", 500});
    };
    const auto r = Poller::run(hooks, h.options());
    CHECK(r.outcome == JobPollOutcome::StartFailed);
    CHECK(h.polls == 0);
    CHECK(h.cancelled.empty());
}

TEST_CASE("JobPoller: an existing job id skips the start hook", "[jobpoller]")
{
    Harness h;
    auto hooks = h.hooks([] { return status("completed"); });
    hooks.start = nullptr;
    const auto r = Poller::run(hooks, h.options(), "job-7");
    CHECK(r.finished());
    CHECK(r.jobId == "job-7");
}
