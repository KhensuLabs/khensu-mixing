// tests/shared-core/test_service_health_tracker.cpp
// Audit ARCH-05: the plugin previously only tracked a raw bool "is the
// service reachable right now", which can't tell "never launched" apart
// from "was there, now gone" - both read as the same "Offline" label. These
// cover the tri-state transitions this pure tracker adds.
#include <catch2/catch_test_macros.hpp>
#include <khensu/ipc/ServiceHealthTracker.h>

using namespace khensu::ipc;

TEST_CASE("A freshly constructed tracker starts NeverConnected", "[service-health]")
{
    ServiceHealthTracker tracker;
    CHECK(tracker.status() == ServiceHealthStatus::NeverConnected);
    CHECK_FALSE(tracker.everConnected());
    CHECK(tracker.consecutiveFailures() == 0);
}

TEST_CASE("Repeated failed ticks before any success stay NeverConnected, not Lost", "[service-health]")
{
    ServiceHealthTracker tracker;
    CHECK(tracker.recordTick(false) == ServiceHealthStatus::NeverConnected);
    CHECK(tracker.recordTick(false) == ServiceHealthStatus::NeverConnected);
    CHECK(tracker.consecutiveFailures() == 2);
    CHECK_FALSE(tracker.everConnected());
}

TEST_CASE("A successful tick reports Connected and resets the failure count", "[service-health]")
{
    ServiceHealthTracker tracker;
    tracker.recordTick(false);
    CHECK(tracker.recordTick(true) == ServiceHealthStatus::Connected);
    CHECK(tracker.everConnected());
    CHECK(tracker.consecutiveFailures() == 0);
}

TEST_CASE("A failure after a prior success reports Lost, not NeverConnected", "[service-health]")
{
    ServiceHealthTracker tracker;
    tracker.recordTick(true);
    CHECK(tracker.recordTick(false) == ServiceHealthStatus::Lost);
    CHECK(tracker.everConnected());
    CHECK(tracker.consecutiveFailures() == 1);
}

TEST_CASE("consecutiveFailures accumulates across a sustained outage after a prior success", "[service-health]")
{
    ServiceHealthTracker tracker;
    tracker.recordTick(true);
    tracker.recordTick(false);
    tracker.recordTick(false);
    CHECK(tracker.recordTick(false) == ServiceHealthStatus::Lost);
    CHECK(tracker.consecutiveFailures() == 3);
}

TEST_CASE("Recovery after Lost returns to Connected", "[service-health]")
{
    ServiceHealthTracker tracker;
    tracker.recordTick(true);
    tracker.recordTick(false);
    CHECK(tracker.recordTick(true) == ServiceHealthStatus::Connected);
    CHECK(tracker.consecutiveFailures() == 0);
}
