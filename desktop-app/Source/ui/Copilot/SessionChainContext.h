// Session target -> mixer-chain generation context. Kept header-only so the
// normalization remains deterministic and directly unit-testable.
#pragma once

#include <JuceHeader.h>
#include <khensu/targets/CreativeTarget.h>

namespace khensu {

struct SessionChainContext {
    juce::String genre;
    juce::String mood;
    juce::String energy;
    double bpm = 0.0;
};

inline SessionChainContext sessionChainContextForTarget(const CreativeTarget& target)
{
    const auto genreId = juce::String(target.primary_genre).toLowerCase();
    juce::String genre = "Pop";
    if (genreId.contains("rap") || genreId.contains("hip-hop") || genreId.contains("trap")) genre = "Hip-Hop";
    else if (genreId.contains("house") || genreId.contains("edm") || genreId.contains("techno") || genreId.contains("dance"))
        genre = "Electronic";
    else if (genreId.contains("r&b") || genreId.contains("soul")) genre = "R&B";
    else if (genreId.contains("rock") || genreId.contains("punk")) genre = "Rock";
    else if (genreId.contains("metal")) genre = "Metal";
    else if (genreId.contains("ambient")) genre = "Ambient";
    else if (genreId.contains("acoustic") || genreId.contains("folk") || genreId.contains("country")) genre = "Acoustic";
    else if (genreId.contains("jazz")) genre = "Jazz";
    else if (genreId.contains("classical") || genreId.contains("orchestral")) genre = "Classical";

    const auto moodId = juce::String(target.mood).toLowerCase();
    juce::String mood = "Modern";
    if (moodId.contains("dream") || moodId.contains("melanch") || moodId.contains("ethereal")) mood = "Dreamy";
    else if (moodId.contains("aggress") || moodId.contains("driv") || moodId.contains("intense")) mood = "Aggressive";
    else if (moodId.contains("warm") || moodId.contains("sultry") || moodId.contains("intimate")) mood = "Warm";
    else if (moodId.contains("bright") || moodId.contains("uplift") || moodId.contains("euphor")) mood = "Uplifting";
    else if (moodId.contains("dark")) mood = "Dark";
    else if (moodId.contains("vintage") || moodId.contains("retro")) mood = "Vintage";

    return {genre, mood, target.energy < 35.0f ? "Low" : target.energy < 70.0f ? "Mid" : "High", target.bpm};
}

} // namespace khensu
