// shared-core/include/khensu/recommendations/ParameterLabels.h
#pragma once
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>

namespace khensu::parameter_labels {

/// A user-facing label and unit for a Recommendation::suggested_parameters
/// key (UX-6: the plugin's Apply dialog listed raw keys like "comp_ratio").
/// Known keys have a written label; any other key is turned into words and
/// its unit suffix (_hz, _db, _dbtp, _ms, _pct) into a unit.
struct Label {
    std::string name; ///< "Compressor ratio"
    std::string unit; ///< "Hz", "dB", ":1", "" ...
};

inline Label labelFor(const std::string& key)
{
    static const std::pair<const char*, Label> kKnown[] = {
        {"comp_ratio", {"Compressor ratio", ":1"}},
        {"comp_threshold_db", {"Compressor threshold", "dB"}},
        {"comp_attack_ms", {"Compressor attack", "ms"}},
        {"comp_release_ms", {"Compressor release", "ms"}},
        {"comp_makeup_gain_db", {"Compressor make-up gain", "dB"}},
        {"eq_freq_hz", {"EQ frequency", "Hz"}},
        {"eq_gain_db", {"EQ gain", "dB"}},
        {"eq_q", {"EQ Q", ""}},
        {"highpass_hz", {"High-pass cutoff", "Hz"}},
        {"reduce_gain_db", {"Lower the level by", "dB"}},
        {"increase_gain_db", {"Raise the level by", "dB"}},
        {"limiter_gain_db", {"Limiter drive", "dB"}},
        {"true_peak_ceiling_dbtp", {"True-peak ceiling", "dBTP"}},
        {"deess_threshold_db", {"De-esser threshold", "dB"}},
        {"deess_freq_hz", {"De-esser frequency", "Hz"}},
        {"transient_attack_db", {"Transient attack", "dB"}},
        {"saturation_drive_db", {"Saturation drive", "dB"}},
        {"stereo_width", {"Stereo width", ""}},
        {"reverb_wet", {"Reverb send", ""}},
        {"reverb_size", {"Reverb size", ""}},
        {"reverb_predelay_ms", {"Reverb pre-delay", "ms"}},
    };
    for (const auto& [k, label] : kKnown)
        if (key == k) return label;

    // Generic: "<words>_<unit>" -> "Words", "<unit>".
    static const std::pair<const char*, const char*> kUnits[] = {{"_dbtp", "dBTP"}, {"_db", "dB"}, {"_hz", "Hz"},
                                                                 {"_ms", "ms"},     {"_pct", "%"}, {"_sec", "s"}};
    std::string stem = key, unit;
    for (const auto& [suffix, u] : kUnits) {
        const std::string s(suffix);
        if (stem.size() > s.size() && stem.compare(stem.size() - s.size(), s.size(), s) == 0) {
            stem.erase(stem.size() - s.size());
            unit = u;
            break;
        }
    }
    for (auto& c : stem)
        if (c == '_') c = ' ';
    if (!stem.empty() && stem[0] >= 'a' && stem[0] <= 'z') stem[0] = static_cast<char>(stem[0] - 'a' + 'A');
    return {stem, unit};
}

/// "Compressor ratio: 2.5:1", "EQ frequency: 3000 Hz", "EQ Q: 0.71".
inline std::string describe(const std::string& key, float value)
{
    const auto label = labelFor(key);
    char number[32];
    const double rounded = std::round(static_cast<double>(value) * 100.0) / 100.0;
    if (std::fabs(rounded - std::round(rounded)) < 1e-9)
        std::snprintf(number, sizeof number, "%.0f", rounded);
    else
        std::snprintf(number, sizeof number, "%.2f", rounded);
    std::string text = label.name + ": " + number;
    if (!label.unit.empty()) text += (label.unit[0] == ':' || label.unit == "%" ? "" : " ") + label.unit;
    return text;
}

} // namespace khensu::parameter_labels
