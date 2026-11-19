// tests/ui/AutoScanCompletionTest.cpp
// The results gate waits for the Dashboard's auto-run scans, so each one's
// completion report must be exact: once when it went async, never when it
// didn't start, never synchronously, never for a manual run.
#include <catch2/catch_test_macros.hpp>
#include "ui/Dashboard/AutoScanCompletion.h"

using khensu::AutoScanCompletion;

TEST_CASE("AutoScanCompletion: a started scan reports exactly once", "[resultsgate]")
{
    AutoScanCompletion scan;
    bool running = false;
    int done = 0;
    const bool started = scan.begin([&] { ++done; }, [&] { running = true; }, [&] { return running; });
    CHECK(started);
    CHECK(done == 0); // not synchronously
    CHECK(scan.pending());
    scan.finish();
    CHECK(done == 1);
    scan.finish(); // a later (manual) run's finish must not fire it again
    CHECK(done == 1);
    CHECK_FALSE(scan.pending());
}

TEST_CASE("AutoScanCompletion: a scan that did not start never reports", "[resultsgate]")
{
    AutoScanCompletion scan;
    int done = 0;
    // e.g. no active session / no reference: the run returns before going async.
    CHECK_FALSE(scan.begin([&] { ++done; }, [] {}, [] { return false; }));
    scan.finish();
    CHECK(done == 0);
}

TEST_CASE("AutoScanCompletion: a scan that ends inside begin() is not counted", "[resultsgate]")
{
    AutoScanCompletion scan;
    int done = 0;
    bool started = true;
    started = scan.begin([&] { ++done; }, [&] { scan.finish(); }, [] { return true; });
    CHECK_FALSE(started); // the caller must not count it as pending
    CHECK(done == 0);     // and onDone never fires synchronously
}

TEST_CASE("AutoScanCompletion: a manual run never reports", "[resultsgate]")
{
    AutoScanCompletion scan;
    scan.finish(); // a button-started scan ends: nothing pending, no crash
    CHECK_FALSE(scan.pending());
}
