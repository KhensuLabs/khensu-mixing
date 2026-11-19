// tests/ui/ServiceReadyWaitTest.cpp
// The deep pass waits for a busy analysis engine instead of skipping on one
// missed /health probe (ServiceReadyWait.h).
#include <catch2/catch_test_macros.hpp>
#include "analysis/ServiceReadyWait.h"

using khensu::service_ready::waitForService;

namespace {
struct FakeClock {
    int now = 0;
    std::function<int()> elapsed() { return [this] { return now; }; }
    std::function<void(int)> sleep() { return [this](int ms) { now += ms; }; }
};
} // namespace

TEST_CASE("Service wait: a service that answers late is still used", "[service-wait]")
{
    FakeClock clock;
    int calls = 0;
    const auto r = waitForService([&] { return ++calls >= 4; }, [] { return false; }, clock.elapsed(), clock.sleep());
    CHECK(r.available);
    CHECK(r.probes == 4);
    CHECK(r.waitedMs == 1500); // three 500 ms retries
}

TEST_CASE("Service wait: gives up after the window, and a cancel stops it early", "[service-wait]")
{
    FakeClock clock;
    const auto down = waitForService([] { return false; }, [] { return false; }, clock.elapsed(), clock.sleep(),
                                     8000, 500);
    CHECK_FALSE(down.available);
    CHECK_FALSE(down.cancelled);
    CHECK(down.waitedMs >= 8000);
    CHECK(down.waitedMs < 8500);

    FakeClock clock2;
    int probes = 0;
    const auto cancelled = waitForService([&] { return ++probes, false; }, [&] { return probes >= 2; },
                                          clock2.elapsed(), clock2.sleep());
    CHECK(cancelled.cancelled);
    CHECK_FALSE(cancelled.available);
    CHECK(cancelled.probes == 2);
}

TEST_CASE("Service wait: an available service costs one probe and no wait", "[service-wait]")
{
    FakeClock clock;
    const auto r = waitForService([] { return true; }, [] { return false; }, clock.elapsed(), clock.sleep());
    CHECK(r.available);
    CHECK(r.probes == 1);
    CHECK(r.waitedMs == 0);
}
