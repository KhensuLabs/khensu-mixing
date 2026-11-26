// desktop-app/Source/ui/shared/MonitorStateText.h
// STU-5: how the transport words its monitoring state (header-only, so the
// tests reach it without the transport bar).
#pragma once
#include <JuceHeader.h>
#include "../../services/IPlayback.h"

namespace khensu::monitor_text {

/// The Monitor button's text: "Monitor" for the plain program, else the
/// mode and/or Dim ("Side", "Sub + Dim", "Dim").
inline juce::String buttonText(IPlayback::MonitorMode mode, bool dim)
{
    juce::String text;
    switch (mode) {
    case IPlayback::MonitorMode::Mono:
        text = TRANS("Mono");
        break;
    case IPlayback::MonitorMode::Side:
        text = TRANS("Side");
        break;
    case IPlayback::MonitorMode::Sub:
        text = TRANS("Sub");
        break;
    case IPlayback::MonitorMode::Full:
        break;
    }
    if (dim) text = text.isEmpty() ? TRANS("Dim") : text + " + " + TRANS("Dim");
    return text.isEmpty() ? TRANS("Monitor") : text;
}

/// The amber badge: "" for the plain program (Full, no Dim), else
/// "MONITORING: MONO + DIM" and so on.
inline juce::String badgeText(IPlayback::MonitorMode mode, bool dim)
{
    if (mode == IPlayback::MonitorMode::Full && !dim) return {};
    return TRANS("MONITORING") + ": " + buttonText(mode, dim).toUpperCase();
}

} // namespace khensu::monitor_text
