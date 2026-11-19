#pragma once
#include <JuceHeader.h>
#include <khensu/recommendations/Recommendation.h>
#include <initializer_list>
#include <vector>

namespace khensu::explainer {

/// Finds the highest-severity recommendation whose id starts with one of the
/// given domain code prefixes (e.g. {"lufs_too_loud", "lufs_too_quiet"} for
/// the Loudness panel). Returns nullptr if none of this domain's codes fired.
inline const Recommendation* findTopRecommendation(const std::vector<Recommendation>& recs,
                                                   std::initializer_list<const char*> codePrefixes)
{
    const Recommendation* best = nullptr;
    for (const auto& r : recs) {
        for (const auto* prefix : codePrefixes) {
            if (r.id.rfind(prefix, 0) == 0) {
                if (best == nullptr || r.severity > best->severity) best = &r;
                break;
            }
        }
    }
    return best;
}

/// Appends the live reasoning trace for `rec` (what was detected + the
/// suggested fix) to a panel's static explainer text, so the "?" popup shows
/// why THIS track's reading matters, not just what the metric means in
/// general. Falls back to a clean "no issues" line when nothing fired.
inline juce::String appendReasoningTrace(const juce::String& staticText, const Recommendation* rec)
{
    if (rec == nullptr) return staticText + "\n\nCurrent finding: no issue flagged here for this track.";
    return staticText + "\n\nCurrent finding (" + juce::String(static_cast<int>(rec->severity)) +
           "% severity): " + juce::String(rec->what) + " " + juce::String(rec->suggested_action);
}

/// Describes a single measured metric row in a panel's live explainer block.
/// Pass a list of these to buildMetricsBlock() to generate the "Measured
/// values:" section that is prepended to the panel's static description.
struct MetricRow {
    juce::String label;     ///< Human-readable name, e.g. "True Peak"
    juce::String value;     ///< Formatted value string, e.g. "-0.52"
    juce::String unit;      ///< Unit suffix, e.g. "dBTP" - empty to omit
    juce::String threshold; ///< e.g. "threshold: -1 dBTP" - empty to omit
};

/// Builds a compact "Measured values" block to prepend to a panel's static
/// explainer text, so the '?' popup shows the live numbers alongside the
/// general explanation of what the metric means.
/// Usage: setExplainer(cat, buildMetricsBlock(rows) + staticText);
inline juce::String buildMetricsBlock(std::initializer_list<MetricRow> rows)
{
    juce::String s = "Measured values:\n";
    for (const auto& r : rows) {
        s += "  " + r.label + ": " + r.value;
        if (r.unit.isNotEmpty()) s += " " + r.unit;
        if (r.threshold.isNotEmpty()) s += "  (" + r.threshold + ")";
        s += "\n";
    }
    return s + "\n";
}

} // namespace khensu::explainer
