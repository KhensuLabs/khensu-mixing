#pragma once
#include <string>

namespace khensu {

/// A frequency range [low_hz, high_hz] in Hz.
/// THREAD_CONTEXT(AnyThread) - immutable value type, safe everywhere.
struct FrequencyRange {
    float low_hz = 0.0f;
    float high_hz = 0.0f;

    constexpr FrequencyRange() = default;
    constexpr FrequencyRange(float low, float high) noexcept : low_hz(low), high_hz(high) {}

    constexpr bool contains(float hz) const noexcept { return hz >= low_hz && hz <= high_hz; }
    constexpr bool is_valid() const noexcept { return high_hz >= low_hz && low_hz >= 0.0f; }
    constexpr float centre() const noexcept { return (low_hz + high_hz) * 0.5f; }

    // Canonical named bands for recommendation labelling
    static constexpr FrequencyRange sub_bass() { return {20.0f, 80.0f}; }
    static constexpr FrequencyRange bass() { return {80.0f, 250.0f}; }
    static constexpr FrequencyRange low_mid() { return {250.0f, 500.0f}; }
    static constexpr FrequencyRange mid() { return {500.0f, 2000.0f}; }
    static constexpr FrequencyRange upper_mid() { return {2000.0f, 6000.0f}; }
    static constexpr FrequencyRange presence() { return {3000.0f, 8000.0f}; }
    static constexpr FrequencyRange air() { return {8000.0f, 20000.0f}; }
    static constexpr FrequencyRange full_range() { return {20.0f, 20000.0f}; }

    std::string to_label() const
    {
        auto fmt = [](float hz) -> std::string {
            if (hz >= 1000.0f) return std::to_string(static_cast<int>(hz / 100) / 10.0f).substr(0, 4) + "kHz";
            return std::to_string(static_cast<int>(hz)) + "Hz";
        };
        return fmt(low_hz) + "-" + fmt(high_hz);
    }

    constexpr bool operator==(const FrequencyRange& o) const noexcept
    {
        return low_hz == o.low_hz && high_hz == o.high_hz;
    }
};

} // namespace khensu
