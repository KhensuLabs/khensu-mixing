// desktop-app/Source/services/AbLevelOffset.h
// UI-6: how loud B is against A, and whether playback evens it out - what
// the A/B control says ("B is -1.8 dB vs A (matched)").
#pragma once
#include <optional>

namespace khensu {

struct AbLevelOffset {
    bool available = false;           ///< there is a B side to compare
    bool measuring = false;           ///< B's loudness is still being measured (B is held muted)
    std::optional<float> bMinusADb;   ///< B's integrated loudness minus A's, when both are known
    /// Playback evens the two out. Only true when A's AND B's loudness are
    /// known - without A's there is nothing to match B to.
    bool matched = false;
};

} // namespace khensu
