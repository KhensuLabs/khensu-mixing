// tests/ui/ServiceWaitPolicyTest.cpp
// P0-5: Intake waits while the service is still starting (a first-run
// install takes minutes) and keeps the 15 s budget only while nothing runs.
#include <catch2/catch_test_macros.hpp>
#include "ui/SessionIntake/ServiceWaitPolicy.h"

using khensu::PythonStatus;
using namespace khensu::service_wait;

TEST_CASE("Intake keeps waiting while the service process is starting (P0-5)", "[intake][p0-5]")
{
    // A 2-minute first-run bootstrap: never given up on while it is starting.
    CHECK(decide(PythonStatus::Starting, 0.0, 120.0) == Decision::KeepWaiting);
    CHECK(decide(PythonStatus::Starting, 14.0, 300.0) == Decision::KeepWaiting);
    // ...but not forever.
    CHECK(decide(PythonStatus::Starting, 0.0, kStartingCapSeconds) == Decision::GiveUp);
}

TEST_CASE("Intake keeps the 15 s budget while nothing runs, and stops when the app gave up", "[intake][p0-5]")
{
    CHECK(decide(PythonStatus::Stopped, 14.5, 200.0) == Decision::KeepWaiting);
    CHECK(decide(PythonStatus::Stopped, 15.0, 15.0) == Decision::GiveUp);
    // Healthy by status but not answering this probe yet: same budget.
    CHECK(decide(PythonStatus::Running, 3.0, 3.0) == Decision::KeepWaiting);
    CHECK(decide(PythonStatus::GaveUp, 0.0, 0.0) == Decision::GiveUp);
}
