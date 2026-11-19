#pragma once

#include <cstdint>
#include <string>

namespace khensu {

/// Identity captured before a background audio read starts. A completion is
/// safe to publish only when every part still matches the current UI state.
/// This is intentionally a pure value type so delayed-reader races can be
/// tested without constructing AppController or starting its services.
struct BackgroundReadKey final {
    std::string sessionId;
    std::string filePath;
    std::string itemId;
    uint64_t generation = 0;

    bool matches(const BackgroundReadKey& current) const noexcept
    {
        return sessionId == current.sessionId
            && filePath == current.filePath
            && itemId == current.itemId
            && generation == current.generation;
    }
};

} // namespace khensu
