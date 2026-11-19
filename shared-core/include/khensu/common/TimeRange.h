#pragma once
#include <algorithm>
#include <string>

namespace khensu {

/// A half-open time range [start_sec, end_sec) in seconds.
/// THREAD_CONTEXT(AnyThread) - immutable value type, safe everywhere.
struct TimeRange {
    float start_sec = 0.0f;
    float end_sec = 0.0f;

    constexpr TimeRange() = default;
    constexpr TimeRange(float start, float end) noexcept : start_sec(start), end_sec(end) {}

    constexpr float duration() const noexcept { return end_sec - start_sec; }
    constexpr bool contains(float t) const noexcept { return t >= start_sec && t < end_sec; }
    constexpr bool is_valid() const noexcept { return end_sec >= start_sec; }

    // Returns a human-readable label, e.g. "0:32-1:04"
    std::string to_label() const
    {
        auto fmt = [](float s) -> std::string {
            int m = static_cast<int>(s) / 60;
            int sec = static_cast<int>(s) % 60;
            return std::to_string(m) + ":" + (sec < 10 ? "0" : "") + std::to_string(sec);
        };
        return fmt(start_sec) + "-" + fmt(end_sec);
    }

    constexpr bool operator==(const TimeRange& o) const noexcept
    {
        return start_sec == o.start_sec && end_sec == o.end_sec;
    }
};

} // namespace khensu
