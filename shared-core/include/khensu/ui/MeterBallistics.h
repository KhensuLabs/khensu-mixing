#pragma once
// UI-5: display ballistics for the studio meters, kept apart from the
// components so they can be tested without a window. Every function takes
// the elapsed time `dt` (seconds) since the previous frame, so the meters
// move at the same speed at 30, 60 or 144 fps.
// THREAD_CONTEXT(UIThread) - plain value types, no locking.

#include <algorithm>
#include <cmath>

namespace khensu::ui::meter {

/// A dB value mapped onto 0..1 of a scale [minDb, maxDb], clamped.
inline float proportion(float db, float minDb, float maxDb) noexcept
{
    if (!std::isfinite(db) || maxDb <= minDb) return 0.0f;
    return std::clamp((db - minDb) / (maxDb - minDb), 0.0f, 1.0f);
}

/// Peak-programme ballistics: an instant rise and a linear fall in dB per
/// second. The default is IEC 60268-18 type I (20 dB in 1.7 s).
struct PeakFall {
    float floorDb = -70.0f;
    float fallDbPerSec = 20.0f / 1.7f;
    float value = -70.0f;

    float update(float inputDb, double dt) noexcept
    {
        if (!std::isfinite(inputDb) || inputDb < floorDb) inputDb = floorDb;
        if (inputDb >= value)
            value = inputDb;
        else
            value = std::max(inputDb, value - fallDbPerSec * static_cast<float>(std::max(0.0, dt)));
        return value;
    }
    void reset() noexcept { value = floorDb; }
};

/// A peak-hold marker: holds the highest value for `holdSeconds`, then
/// falls at `fallDbPerSec` until the signal meets it again.
struct PeakHold {
    float floorDb = -70.0f;
    float holdSeconds = 2.0f;
    float fallDbPerSec = 20.0f;
    float value = -70.0f;
    double heldFor = 0.0;

    float update(float inputDb, double dt) noexcept
    {
        if (!std::isfinite(inputDb) || inputDb < floorDb) inputDb = floorDb;
        dt = std::max(0.0, dt);
        if (inputDb >= value) {
            value = inputDb;
            heldFor = 0.0;
        } else if ((heldFor += dt) > holdSeconds) {
            value = std::max(inputDb, value - fallDbPerSec * static_cast<float>(dt));
        }
        return value;
    }
    void reset() noexcept
    {
        value = floorDb;
        heldFor = 0.0;
    }
};

/// The highest value seen since the last reset (the numeric max-hold).
struct MaxHold {
    float floorDb = -200.0f;
    float value = -200.0f;

    float update(float inputDb) noexcept
    {
        if (std::isfinite(inputDb)) value = std::max(value, inputDb);
        return value;
    }
    void reset() noexcept { value = floorDb; }
    bool hasValue() const noexcept { return value > floorDb; }
};

/// One-pole smoothing with separate rise and fall time constants
/// (seconds): each frame moves by 1 - exp(-dt / tau) of the distance.
struct Smoother {
    float riseSeconds = 0.05f;
    float fallSeconds = 0.30f;
    float value = 0.0f;
    bool primed = false;

    float update(float target, double dt) noexcept
    {
        if (!std::isfinite(target)) return value;
        if (!primed) {
            snap(target);
            return value;
        }
        const double tau = target > value ? riseSeconds : fallSeconds;
        const double a = tau <= 0.0 ? 1.0 : 1.0 - std::exp(-std::max(0.0, dt) / tau);
        value += static_cast<float>(a) * (target - value);
        return value;
    }
    void snap(float target) noexcept
    {
        value = target;
        primed = true;
    }
};

} // namespace khensu::ui::meter
