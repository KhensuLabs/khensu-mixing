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
#include "../../services/DeepPassState.h"

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

/// The same state in two words, for a header where the full line would be
/// ellipsized (UX2-3): "Engine running", "Engine starting...".
inline juce::String shortStateLine(PythonStatus status)
{
    switch (status) {
    case PythonStatus::Running: return TRANS("Engine running");
    case PythonStatus::GaveUp: return TRANS("Engine unavailable");
    case PythonStatus::Starting: return TRANS("Engine starting...");
    case PythonStatus::Stopped: break;
    }
    return TRANS("Engine stopped");
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

/// The Dashboard's status line for a finished analysis, by the real reason
/// its deep half is there or missing (OVH-1). The old line blamed an
/// unreachable engine for all of them - while the sidebar said "running".
inline juce::String deepPassLine(DeepPassState state)
{
    switch (state) {
    case DeepPassState::Complete: return TRANS("Analysis complete.");
    case DeepPassState::Off: return TRANS("Fast analysis only - deep analysis is off for this session.");
    case DeepPassState::SkippedByUser: return TRANS("Fast analysis only - you skipped the deep analysis.");
    case DeepPassState::Failed: return TRANS("Fast analysis only - the deep analysis failed.");
    case DeepPassState::EngineUnavailable:
        // FN2-2b: the native findings are complete and on screen; the deep
        // pass re-runs in place once the engine is running.
        return TRANS("Fast check - deeper checks run when the analysis engine is ready.");
    }
    return TRANS("Analysis complete.");
}

/// Whether the Dashboard offers "Re-run deep analysis" for this state: not
/// when there is nothing missing, nor when the user turned it off.
inline bool deepPassOffersRerun(DeepPassState state) noexcept
{
    return state == DeepPassState::Failed || state == DeepPassState::EngineUnavailable
           || state == DeepPassState::SkippedByUser;
}

/// What a panel shows in place of service data while the engine isn't running.
inline juce::String waitingPlaceholder()
{
    return TRANS("Waiting for the analysis engine - this fills in once it is running.");
}

} // namespace khensu::service_status
