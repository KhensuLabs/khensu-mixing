// desktop-app/Source/ui/shared/ServiceStatusText.h
// One wording for the analysis engine's state (UX-7). The sidebar, the
// Dashboard health widget, the app-wide notice bar and the panels that wait
// on the service used to describe the same "starting" state five different
// ways ("starting...", "Service: Starting...", "Service info unavailable
// (service offline?)", "Could not load targets...", "Run 'Restart analysis
// service'"). They all read it from here.
#pragma once
#include <JuceHeader.h>
#include "PythonStatus.h"

namespace khensu::service_status {

/// The short state, e.g. for the sidebar row: "Analysis engine: starting...".
inline juce::String stateLine(PythonStatus status, int restartAttempt = 0)
{
    switch (status) {
    case PythonStatus::Running: return TRANS("Analysis engine: running");
    case PythonStatus::GaveUp: return TRANS("Analysis engine: unavailable");
    case PythonStatus::Starting:
        return restartAttempt > 0
                   ? TRANS("Analysis engine: restarting (attempt %d)...").replace("%d", juce::String(restartAttempt))
                   : TRANS("Analysis engine: starting...");
    case PythonStatus::Stopped: break;
    }
    return TRANS("Analysis engine: stopped");
}

/// The app-wide banner while the engine isn't running; empty when it is.
inline juce::String bannerText(PythonStatus status, int restartAttempt = 0)
{
    switch (status) {
    case PythonStatus::Running: return {};
    case PythonStatus::Starting:
        return restartAttempt > 0 ? TRANS("The analysis engine is restarting. Results fill in once it is running.")
                                  : TRANS("The analysis engine is starting. Results fill in once it is running.");
    case PythonStatus::GaveUp: return TRANS("The analysis engine could not start.");
    case PythonStatus::Stopped: break;
    }
    return TRANS("The analysis engine is not running.");
}

/// Whether the banner offers a restart (the engine is down, not on its way up).
inline bool bannerOffersRestart(PythonStatus status) noexcept
{
    return status == PythonStatus::Stopped || status == PythonStatus::GaveUp;
}

/// What a panel shows in place of service data while the engine isn't running.
inline juce::String waitingPlaceholder()
{
    return TRANS("Waiting for the analysis engine - this fills in once it is running.");
}

} // namespace khensu::service_status
