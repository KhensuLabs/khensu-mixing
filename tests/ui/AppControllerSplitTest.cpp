// tests/ui/AppControllerSplitTest.cpp
// Pins the logic extracted from AppController (audit refactor "Split
// AppController"): the service-health state machine and the A/B
// loudness-matched playback gain.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include "session/PlaybackSourceController.h"
#include "session/ServiceHealthMonitor.h"

using khensu::PlaybackSourceController;
using khensu::ServiceHealthMonitor;

TEST_CASE("ServiceHealthMonitor fires reanalyze once on the healthy edge", "[split][health]")
{
    ServiceHealthMonitor m;
    auto d = m.onHealthResult(false, true, false, true);
    CHECK(d.status == ServiceHealthMonitor::Status::Starting);
    CHECK_FALSE(d.reanalyze);
    d = m.onHealthResult(true, true, false, true);
    CHECK(d.status == ServiceHealthMonitor::Status::Running);
    CHECK(d.reanalyze);
    CHECK(d.dispatchOutbox);
    d = m.onHealthResult(true, true, false, true);
    CHECK_FALSE(d.reanalyze); // steady healthy: no repeat
    m.onHealthResult(false, false, false, false);
    d = m.onHealthResult(true, true, false, true);
    CHECK(d.reanalyze); // recovered again after an outage
}

TEST_CASE("ServiceHealthMonitor restarts a live service that stopped listening", "[split][health]")
{
    ServiceHealthMonitor m;
    // Cold start without a listener is never "wedged": it has not answered yet.
    for (int i = 0; i < 2 * ServiceHealthMonitor::kWedgedProbes; ++i) {
        const auto d = m.onHealthResult(false, true, false, false, /*portListening*/ false);
        CHECK(d.status == ServiceHealthMonitor::Status::Starting);
        CHECK_FALSE(d.restartWedged);
    }
    m.onHealthResult(true, true, false, false); // answered once

    // Alive, socket closed (real run 2026-11-14): restart after the threshold.
    int restarts = 0;
    for (int i = 0; i < ServiceHealthMonitor::kWedgedProbes; ++i) {
        const auto d = m.onHealthResult(false, true, false, false, false);
        CHECK(d.status == ServiceHealthMonitor::Status::Stopped);
        if (d.restartWedged) ++restarts;
    }
    CHECK(restarts == 1);
    // The relaunch is a cold start: no second restart while it boots.
    for (int i = 0; i < 2 * ServiceHealthMonitor::kWedgedProbes; ++i)
        CHECK_FALSE(m.onHealthResult(false, true, false, false, false).restartWedged);
}

TEST_CASE("ServiceHealthMonitor leaves a busy but listening service alone", "[split][health]")
{
    ServiceHealthMonitor m;
    m.onHealthResult(true, true, false, false);
    for (int i = 0; i < 3 * ServiceHealthMonitor::kWedgedProbes; ++i) {
        const auto d = m.onHealthResult(false, true, false, false, /*portListening*/ true);
        CHECK(d.status == ServiceHealthMonitor::Status::Starting);
        CHECK_FALSE(d.restartWedged);
    }
    // A listener coming back resets the count.
    for (int i = 0; i < ServiceHealthMonitor::kWedgedProbes - 1; ++i)
        CHECK_FALSE(m.onHealthResult(false, true, false, false, false).restartWedged);
    m.onHealthResult(false, true, false, false, true);
    CHECK_FALSE(m.onHealthResult(false, true, false, false, false).restartWedged);
}

TEST_CASE("ServiceHealthMonitor notifies 'gave up' once per outage", "[split][health]")
{
    ServiceHealthMonitor m;
    auto d = m.onHealthResult(false, false, true, false);
    CHECK(d.status == ServiceHealthMonitor::Status::GaveUp);
    CHECK(d.notifyGaveUp);
    d = m.onHealthResult(false, false, true, false);
    CHECK_FALSE(d.notifyGaveUp);
    m.resetGaveUpNotice(); // manual restart
    d = m.onHealthResult(false, false, true, false);
    CHECK(d.notifyGaveUp);
}

TEST_CASE("ServiceHealthMonitor restarts a stopped service only when backoff allows", "[split][health]")
{
    ServiceHealthMonitor m;
    CHECK(m.onHealthResult(false, false, false, true).attemptRestart);
    CHECK_FALSE(m.onHealthResult(false, false, false, false).attemptRestart);
}

TEST_CASE("PlaybackSourceController attenuates the louder A/B side", "[split][ab]")
{
    PlaybackSourceController p;
    p.userVolume = 1.0f;
    p.processedPath = "b.wav";
    p.processedLufs = -10.0f;
    PlaybackSourceController::GainContext ctx;
    ctx.originalLufs = -16.0f;
    ctx.originalPath = "a.wav";

    ctx.loadedPath = "b.wav"; // B is 6 dB louder -> B down 6 dB
    CHECK(p.gainFor(ctx) == Catch::Approx(0.501f).margin(0.002f));
    ctx.loadedPath = "a.wav"; // A is quieter -> unity
    CHECK(p.gainFor(ctx) == Catch::Approx(1.0f));

    p.processedLufs = -20.0f; // now B is quieter: A comes down instead
    CHECK(p.gainFor(ctx) == Catch::Approx(0.631f).margin(0.002f));
    ctx.loadedPath = "vocals.wav"; // a soloed stem plays unmatched
    CHECK(p.gainFor(ctx) == Catch::Approx(1.0f));
}

TEST_CASE("PlaybackSourceController holds B muted until it is measured", "[split][ab][audit038]")
{
    PlaybackSourceController p;
    p.userVolume = 0.5f;
    p.processedPath = "b.wav";
    PlaybackSourceController::GainContext ctx;
    ctx.originalLufs = -20.0f;
    ctx.originalPath = "a.wav";
    ctx.loadedPath = "b.wav";
    CHECK(p.awaitingProcessedLufs());
    CHECK(p.gainFor(ctx) == 0.0f); // never an unmatched (possibly much louder) B
    ctx.loadedPath = "a.wav";
    CHECK(p.gainFor(ctx) == Catch::Approx(0.5f)); // A plays unmatched meanwhile

    p.processedLufs = -14.0f; // measured: B 6 dB louder -> B down 6 dB
    ctx.loadedPath = "b.wav";
    CHECK(p.gainFor(ctx) == Catch::Approx(0.5f * 0.501f).margin(0.002f));

    p.processedLufs.reset();
    p.processedLufsUnavailable = true; // unmeasurable: play unmatched, not muted forever
    CHECK(p.gainFor(ctx) == Catch::Approx(0.5f));

    p.resetForSession();
    CHECK_FALSE(p.processedLufsUnavailable);
    CHECK(p.gainFor(ctx) == Catch::Approx(0.5f)); // no A/B -> just the volume
}
