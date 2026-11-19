// desktop-app/Source/ui/SessionIntake/ServiceWaitPolicy.h
#pragma once
#include "../shared/PythonStatus.h"

namespace khensu::service_wait {

/// How long Intake waits for the analysis service before a drop is analysed
/// (P0-5). A first-run bootstrap (venv + pip install) takes 1-3 minutes, and
/// Intake used to give up after 15 s while the app knew the process was alive
/// and still installing. Now:
///  - the app has given up on the service: stop at once;
///  - the process is alive but not healthy yet (Starting): keep waiting, up to
///    kStartingCapSeconds in all;
///  - nothing is running: keep waiting for kDownSeconds, the old budget, which
///    only counts while the service is not starting.
inline constexpr int kDownSeconds = 15;
inline constexpr int kStartingCapSeconds = 10 * 60;

enum class Decision { KeepWaiting, GiveUp };

inline Decision decide(PythonStatus status, double secondsDown, double secondsTotal) noexcept
{
    if (status == PythonStatus::GaveUp) return Decision::GiveUp;
    if (status == PythonStatus::Starting)
        return secondsTotal < kStartingCapSeconds ? Decision::KeepWaiting : Decision::GiveUp;
    return secondsDown < kDownSeconds ? Decision::KeepWaiting : Decision::GiveUp;
}

} // namespace khensu::service_wait
