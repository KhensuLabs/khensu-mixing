#pragma once
#include <algorithm>
#include <string>
#include <vector>

namespace khensu {

struct PlatformLoudnessTarget {
    std::string name;
    float lufs;
    float dbtp;
    /// Whether the platform's playback normalisation ever turns a QUIET track
    /// up (UIX-12). YouTube, Apple Music, Tidal and Amazon only turn loud
    /// tracks down; Spotify (normal mode) also raises quiet ones, but only as
    /// far as its true-peak headroom allows - it does not limit to get louder.
    bool turnsUp = false;
};

inline const std::vector<PlatformLoudnessTarget>& getPlatformLoudnessTargets()
{
    static const std::vector<PlatformLoudnessTarget> targets = {{"Spotify", -14.0f, -1.0f, true},
                                                                {"Apple Music", -16.0f, -1.0f, false},
                                                                {"YouTube", -14.0f, -1.0f, false},
                                                                {"Tidal", -14.0f, -1.0f, false},
                                                                {"Amazon Music", -14.0f, -1.0f, false}};
    return targets;
}

/// The gain (dB) platform `p`'s loudness normalisation applies at playback to
/// a track measuring `lufs` integrated with `truePeakDbtp`. Negative = turned
/// down. Upward gain only on platforms that turn up, and never past the
/// platform's true-peak ceiling (UIX-12: the preview used to apply
/// `target - lufs` raw, boosting a -20 LUFS mix 6 dB "as YouTube hears it"
/// and pushing it into clipping). An unmeasurable / silent level gets 0.
inline float platformPlaybackGainDb(const PlatformLoudnessTarget& p, float lufs, float truePeakDbtp) noexcept
{
    if (!(lufs > -70.0f)) return 0.0f; // silent or not measured (NaN fails the test too)
    const float adj = p.lufs - lufs;
    if (adj <= 0.0f) return adj;
    if (!p.turnsUp) return 0.0f;
    const float headroom = p.dbtp - truePeakDbtp;
    return std::max(0.0f, std::min(adj, headroom));
}

} // namespace khensu
