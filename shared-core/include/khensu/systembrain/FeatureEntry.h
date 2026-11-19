#pragma once
#include <string>
#include <vector>

namespace khensu {

/// Optional per-card mini-visualization hint.
/// THREAD_CONTEXT(AnyThread) - plain data struct.
struct FeatureVisualization {
    std::string type; ///< "bar", "dot", "percentile", "severity", "presence", "sparkline", "icon", "none"
    float min = 0.0f;
    float max = 1.0f;
    std::vector<std::string> labels; ///< bucket labels for presence/icon modes
    std::string icon;                ///< icon name for "icon" mode

    bool hasVisualization() const noexcept { return !type.empty() && type != "none"; }
};

/// One entry in the System Brain feature encyclopedia.
/// THREAD_CONTEXT(AnyThread) - plain data struct.
struct FeatureEntry {
    std::string id;
    std::string name;
    std::string category;
    std::string short_description;
    std::string detailed_description;
    std::string logic_and_math; ///< the thresholds/formulas behind the decisions
    std::string why_it_exists;
    std::string when_to_use;
    std::string desktop_location;
    bool plugin_available = false;
    std::vector<std::string> inputs;
    std::vector<std::string> tags;
    std::vector<std::string> related_features;
    FeatureVisualization visualization;

    bool is_valid() const noexcept { return !id.empty(); }
};

} // namespace khensu
