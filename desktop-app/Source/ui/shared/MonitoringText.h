#pragma once
#include <JuceHeader.h>
#include <khensu/recommendations/MonitoringReach.h>

namespace khensu::monitoring_text {

/// NEW-10: the translated label of a monitoring::Profile id (shared-core
/// holds the ids and limits; its English labels never reach the i18n template).
inline juce::String label(const std::string& id)
{
    if (id == "full-range") return TRANS("Full-range monitors or a subwoofer");
    if (id == "headphones") return TRANS("Over-ear headphones");
    if (id == "nearfield") return TRANS("Small studio monitors (5 inch)");
    if (id == "earbuds") return TRANS("Earbuds");
    if (id == "laptop") return TRANS("Laptop speakers");
    if (id == "phone") return TRANS("Phone speaker");
    return TRANS("Not stated");
}

/// The issue row's note for a finding the monitoring cannot reproduce;
/// empty when it can (or nothing was stated).
inline juce::String note(const Recommendation& rec, const std::string& monitoringId)
{
    const auto r = monitoring::reach(rec, monitoringId);
    if (!r.outOfReach) return {};
    return TRANS("Too low to judge on what you listen on (%m): it sits below about %f Hz. Check it on "
                 "headphones, or hear a small speaker with the Dashboard's device preview.")
        .replace("%m", label(monitoringId))
        .replace("%f", juce::String(juce::roundToInt(r.limitHz)));
}

} // namespace khensu::monitoring_text
