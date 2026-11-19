#pragma once
#include <string>

namespace khensu {

/// Classification of a recommendation's primary domain.
enum class RecommendationType {
    Technical,  // Measurable, objective (e.g. true peak exceeds -1 dBTP)
    Perceptual, // Psychoacoustic / listening experience (e.g. harsh 3kHz sibilance)
    Musical,    // Musicological / hook / memorability (e.g. chorus hook too quiet)
    Structural, // Song architecture (e.g. pre-chorus builds tension too slowly)
    Strength,   // Something the mix does RIGHT (spec §4) - severity is always 0;
                // rendered green so wins are as visible as problems
};

inline std::string to_string(RecommendationType t)
{
    switch (t) {
    case RecommendationType::Technical:
        return "Technical";
    case RecommendationType::Perceptual:
        return "Perceptual";
    case RecommendationType::Musical:
        return "Musical";
    case RecommendationType::Structural:
        return "Structural";
    case RecommendationType::Strength:
        return "Strength";
    }
    return "Unknown";
}

inline RecommendationType recommendation_type_from_string(const std::string& s)
{
    if (s == "Perceptual") return RecommendationType::Perceptual;
    if (s == "Musical") return RecommendationType::Musical;
    if (s == "Structural") return RecommendationType::Structural;
    if (s == "Strength") return RecommendationType::Strength;
    return RecommendationType::Technical;
}

} // namespace khensu
