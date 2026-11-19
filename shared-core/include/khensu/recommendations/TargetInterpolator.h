#pragma once
#include <khensu/targets/GenreEntry.h>
#include <khensu/targets/CreativeTarget.h>
#include <algorithm>

namespace khensu {

/// Stateless utility: maps creative target slider value t ∈ [0, 100] to a
/// recommendation firing threshold.
///
/// Core formula:
///   t <= 50: floor + (norm - floor) x (t / 50)
///   t > 50: norm + (ceil - norm) x ((t - 50) / 50)
///
/// t = 0   → floor  (most lenient - only fire for extreme deviations)
/// t = 50  → norm   (fire at genre standard)
/// t = 100 → ceil   (fire even for slight deviations)
///
/// THREAD_CONTEXT(AnyThread) - stateless, all methods static.
class TargetInterpolator {
  public:
    /// Core interpolation primitive. All values unclamped - callers clamp if needed.
    static float interpolate(float t, float floor_val, float norm, float ceil_val) noexcept
    {
        if (t <= 50.0f)
            return floor_val + (norm - floor_val) * (t / 50.0f);
        else
            return norm + (ceil_val - norm) * ((t - 50.0f) / 50.0f);
    }

    /// Hook density firing threshold.
    /// norm = genre.hook_density_norm; floor delta = -0.30; ceil delta = +0.10
    /// Higher hook_emphasis → stricter low bound (threshold rises).
    static float hookDensityThreshold(const GenreEntry& genre, const CreativeTarget& target) noexcept
    {
        const float norm = genre.hook_density_norm;
        const float floor_val = norm - 0.30f;
        const float ceil_val = norm + 0.10f;
        // FIX-9: a 0.95 norm put the strict threshold at 1.05 (fires on every
        // mix) and a 0.20 norm the loose one at -0.10 (never fires).
        return std::clamp(interpolate(target.hook_emphasis, floor_val, norm, ceil_val), 0.05f, 0.95f);
    }

    /// Hook strength firing threshold.
    /// Absolute constants: floor=0.35, norm=0.55, ceil=0.70
    static float hookStrengthThreshold(const CreativeTarget& target) noexcept
    {
        return interpolate(target.hook_emphasis, 0.35f, 0.55f, 0.70f);
    }

    /// Memorability firing threshold.
    /// Absolute constants: floor=0.30, norm=0.50, ceil=0.70
    static float memorabilityThreshold(const CreativeTarget& target) noexcept
    {
        return interpolate(target.memorability_target, 0.30f, 0.50f, 0.70f);
    }

    /// Earworm likelihood firing threshold.
    /// Absolute constants: floor=0.25, norm=0.40, ceil=0.60
    static float earwormThreshold(const CreativeTarget& target) noexcept
    {
        return interpolate(target.memorability_target, 0.25f, 0.40f, 0.60f);
    }

    /// Structural-flow (arc quality) firing threshold, driven by the Intake
    /// "Chorus payoff" slider (desired verse -> chorus contrast).
    /// Absolute constants: floor=0.35, norm=0.50, ceil=0.65 - so the default
    /// payoff of 50 keeps the historical 0.50 threshold.
    static float arcQualityThreshold(const CreativeTarget& target) noexcept
    {
        return interpolate(target.chorus_payoff, 0.35f, 0.50f, 0.65f);
    }

    /// Heaviness extremity firing threshold (bidirectional: fires when |measured - threshold| > 0.20).
    /// norm = genre.heaviness_extremity_norm; floor delta = -0.20; ceil delta = +0.20
    static float heavinessExtremityThreshold(const GenreEntry& genre, const CreativeTarget& target) noexcept
    {
        const float norm = genre.heaviness_extremity_norm;
        const float floor_val = norm - 0.20f;
        const float ceil_val = norm + 0.20f;
        return std::clamp(interpolate(target.heaviness_target, floor_val, norm, ceil_val), 0.05f, 0.95f);
    }

    /// Heaviness weight firing threshold.
    /// norm = genre.heaviness_weight_norm; floor delta = -0.25; ceil delta = +0.15
    static float heavinessWeightThreshold(const GenreEntry& genre, const CreativeTarget& target) noexcept
    {
        const float norm = genre.heaviness_weight_norm;
        const float floor_val = norm - 0.25f;
        const float ceil_val = norm + 0.15f;
        return std::clamp(interpolate(target.heaviness_target, floor_val, norm, ceil_val), 0.05f, 0.95f);
    }
};

} // namespace khensu
