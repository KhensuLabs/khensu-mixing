#pragma once
#include <JuceHeader.h>

namespace khensu {

/// The user-visible terminal summary for a multi-file intake run. Kept pure so
/// a mixed success/failure/cancellation batch is regression-testable without
/// starting the desktop service lifecycle.
inline juce::String formatIntakeQueueCompletion(int total, int failed, int cancelled)
{
    const int analyzed = juce::jmax(0, total - failed - cancelled);
    return "Intake queue complete: " + juce::String(analyzed) + "/" + juce::String(total) + " analyzed" +
           (failed > 0 ? " (" + juce::String(failed) + " failed)" : juce::String()) +
           (cancelled > 0 ? " (" + juce::String(cancelled) + " cancelled)" : juce::String());
}

} // namespace khensu
