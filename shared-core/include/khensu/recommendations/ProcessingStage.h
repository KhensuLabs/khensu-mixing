#pragma once
#include <string_view>

namespace khensu {

/// Identifies which production stage should address a given recommendation.
/// Used to group and sequence recommendations for the engineer.
enum class ProcessingStage {
    Arrangement, ///< Fix in arrangement / composition
    SoundDesign, ///< Fix via sound selection or synthesis
    Edit,        ///< Fix via editing (comping, timing, tuning)
    MixChannel,  ///< Fix on an individual channel in the mix
    MixBus,      ///< Fix on a sub-bus or group in the mix
    Master,      ///< Fix during mastering
    Monitoring,  ///< Fix related to monitoring/room - not an audio problem
    Unknown      ///< Not yet classified (default)
};

/// Returns a stable ASCII string for the stage (safe to store in JSON/DB).
constexpr const char* to_string(ProcessingStage s) noexcept
{
    switch (s) {
    case ProcessingStage::Arrangement:
        return "Arrangement";
    case ProcessingStage::SoundDesign:
        return "SoundDesign";
    case ProcessingStage::Edit:
        return "Edit";
    case ProcessingStage::MixChannel:
        return "MixChannel";
    case ProcessingStage::MixBus:
        return "MixBus";
    case ProcessingStage::Master:
        return "Master";
    case ProcessingStage::Monitoring:
        return "Monitoring";
    case ProcessingStage::Unknown:
        return "Unknown";
    }
    return "Unknown";
}

} // namespace khensu
