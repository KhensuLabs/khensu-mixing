// tests/ui/PythonServiceManagerRestartTest.cpp
// Audit ARCH-06: missing Python, missing launcher, and process-start
// failures previously returned from launchIfNeeded() without incrementing
// the same failure counter spawned-child crashes use, so a pre-spawn
// failure loop could retry beyond the documented kMaxConsecutiveFailures
// give-up threshold. That's already fixed (recordLaunchFailure() covers all
// three branches); these pin it deterministically via the test-only
// override seams (setPythonExeOverrideForTesting/
// setLauncherPathOverrideForTesting/launchIfNeededForTesting) instead of
// depending on the real filesystem/PATH search or the async worker thread.
#include <catch2/catch_test_macros.hpp>
#include "PortListeners.h"
#include "PythonServiceManager.h"
#include <juce_core/juce_core.h>

using namespace khensu;

TEST_CASE("PythonServiceManager counts a missing-Python failure toward the give-up threshold",
          "[python_service_manager]")
{
    PythonServiceManager mgr;
    mgr.setSkipAvailabilityCheckForTesting();
    mgr.setPythonExeOverrideForTesting({}); // empty -> "Python not found" branch

    CHECK(mgr.consecutiveFailureCount() == 0);
    CHECK_FALSE(mgr.launchIfNeededForTesting());
    CHECK(mgr.consecutiveFailureCount() == 1);
    CHECK_FALSE(mgr.hasGivenUp());
}

TEST_CASE("PythonServiceManager counts a missing-launcher failure toward the give-up threshold",
          "[python_service_manager]")
{
    PythonServiceManager mgr;
    mgr.setSkipAvailabilityCheckForTesting();
    // A non-empty exe path passes the isEmpty() check so the missing-launcher
    // branch is reached; it doesn't need to exist since findLauncherScript()
    // is checked (and fails) before any process is ever started. The bogus
    // launcher path stays on the temp directory's own drive rather than an
    // unmounted drive letter - Windows can spend real time (1-2s+) probing
    // network-provider resolution for a drive letter with nothing mapped to
    // it, which made this test needlessly slow.
    mgr.setPythonExeOverrideForTesting("python3");
    mgr.setLauncherPathOverrideForTesting(
        juce::File::getSpecialLocation(juce::File::tempDirectory)
            .getChildFile("khensu_test_nonexistent_dir/launch_service.py"));

    CHECK_FALSE(mgr.launchIfNeededForTesting());
    CHECK(mgr.consecutiveFailureCount() == 1);
}

TEST_CASE("PythonServiceManager counts a process-start failure toward the give-up threshold",
          "[python_service_manager]")
{
    auto tempLauncher = juce::File::getSpecialLocation(juce::File::tempDirectory)
                            .getChildFile("khensu_fake_launch_service.py");
    tempLauncher.replaceWithText("# not actually executed - launchIfNeeded's ChildProcess::start()\n"
                                 "# fails before this file's content ever matters, because the\n"
                                 "# overridden \"python\" executable path does not exist.\n");

    PythonServiceManager mgr;
    mgr.setSkipAvailabilityCheckForTesting();
    // A python path that passes the isEmpty() check but is not a real
    // executable makes juce::ChildProcess::start() itself fail, exercising
    // the third pre-spawn branch without ever spawning a real process. Kept
    // on the temp directory's own drive for the same reason as above.
    mgr.setPythonExeOverrideForTesting(
        juce::File::getSpecialLocation(juce::File::tempDirectory)
            .getChildFile("khensu_test_nonexistent_dir/python.exe")
            .getFullPathName());
    mgr.setLauncherPathOverrideForTesting(tempLauncher);

    CHECK_FALSE(mgr.launchIfNeededForTesting());
    CHECK(mgr.consecutiveFailureCount() == 1);

    tempLauncher.deleteFile();
}

TEST_CASE("PythonServiceManager gives up after kMaxConsecutiveFailures pre-spawn failures in a row",
          "[python_service_manager]")
{
    PythonServiceManager mgr;
    mgr.setSkipAvailabilityCheckForTesting();
    mgr.setPythonExeOverrideForTesting({});

    for (int i = 0; i < PythonServiceManager::kMaxConsecutiveFailures; ++i) {
        CHECK_FALSE(mgr.hasGivenUp());
        CHECK_FALSE(mgr.launchIfNeededForTesting());
    }

    CHECK(mgr.consecutiveFailureCount() == PythonServiceManager::kMaxConsecutiveFailures);
    CHECK(mgr.hasGivenUp());
    CHECK_FALSE(mgr.canAttemptRestart());
}

TEST_CASE("PythonServiceManager::notifyHealthy resets the failure count after pre-spawn failures",
          "[python_service_manager]")
{
    PythonServiceManager mgr;
    mgr.setSkipAvailabilityCheckForTesting();
    mgr.setPythonExeOverrideForTesting({});
    mgr.launchIfNeededForTesting();
    mgr.launchIfNeededForTesting();
    CHECK(mgr.consecutiveFailureCount() == 2);

    mgr.notifyHealthy();
    CHECK(mgr.consecutiveFailureCount() == 0);
    CHECK_FALSE(mgr.hasGivenUp());
}

// The identity gate (AUDIT-020) and the one-slow-/health race: a verified
// service that misses a probe keeps the gate open for kIdentityGraceProbes
// misses - but only while the same process is still the port's sole listener
// - so requests in that window are not failed with "service identity not
// verified". An answered probe always decides at once.
TEST_CASE("PythonServiceManager keeps a verified identity through a few unanswered probes",
          "[python-service][identity]")
{
    using PO = PythonServiceManager::ProbeOutcome;
    int unanswered = 0;
    bool verified = PythonServiceManager::identityAfterProbe(false, PO::Verified, unanswered, true);
    REQUIRE(verified);

    for (int i = 0; i < PythonServiceManager::kIdentityGraceProbes; ++i) {
        verified = PythonServiceManager::identityAfterProbe(verified, PO::NoAnswer, unanswered, true);
        CHECK(verified);
    }
    // One miss too many: the gate closes.
    verified = PythonServiceManager::identityAfterProbe(verified, PO::NoAnswer, unanswered, true);
    CHECK_FALSE(verified);
    // ...and stays closed until a probe verifies again.
    CHECK_FALSE(PythonServiceManager::identityAfterProbe(verified, PO::NoAnswer, unanswered, true));
    verified = PythonServiceManager::identityAfterProbe(verified, PO::Verified, unanswered, true);
    CHECK(verified);
    CHECK(unanswered == 0);
}

TEST_CASE("PythonServiceManager closes the identity gate at once on evidence", "[python-service][identity]")
{
    using PO = PythonServiceManager::ProbeOutcome;
    int unanswered = 0;
    // A listener that answers but is not provably ours.
    CHECK_FALSE(PythonServiceManager::identityAfterProbe(true, PO::Rejected, unanswered, true));
    // The verified process no longer holds the port alone (it exited, or a
    // squatter accepts and stalls): no grace, the token must not reach it.
    CHECK_FALSE(PythonServiceManager::identityAfterProbe(true, PO::NoAnswer, unanswered, false));
    CHECK(unanswered == 0);
    // Never verified: a miss does not open it.
    CHECK_FALSE(PythonServiceManager::identityAfterProbe(false, PO::NoAnswer, unanswered, true));
    // An answered probe resets the miss count.
    unanswered = PythonServiceManager::kIdentityGraceProbes;
    CHECK(PythonServiceManager::identityAfterProbe(false, PO::Verified, unanswered, true));
    CHECK(unanswered == 0);
}

#if JUCE_WINDOWS
// The grace keys on the port owner's PID; prove the lookup finds a real one.
TEST_CASE("PythonServiceManager::listenerPids finds this process listening", "[python-service][identity]")
{
    juce::StreamingSocket listener;
    REQUIRE(listener.createListener(0, "127.0.0.1"));
    const int port = listener.getBoundPort();
    REQUIRE(port > 0);
    const auto pids = PythonServiceManager::listenerPids(port);
    REQUIRE(pids.size() == 1);
    CHECK(pids.front() == khensu::currentProcessId());
    listener.close();
    CHECK(PythonServiceManager::listenerPids(port).empty());
}
#endif
