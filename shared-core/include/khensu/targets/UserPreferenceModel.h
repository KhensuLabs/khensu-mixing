#pragma once
#include <khensu/targets/GenreEntry.h>
#include <khensu/feedback/FeedbackManager.h>
#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

namespace khensu {

/// Model to capture, learn, and apply user-specific mixing target preferences.
///
/// Learns continuously from user accept/reject actions stored via FeedbackManager,
/// and adapts reference targets (GenreEntry) before they are run through the recommendation engine.
/// THREAD_CONTEXT(AnyThread) - database reads/writes are thread-safe under FeedbackManager.
class UserPreferenceModel {
  public:
    UserPreferenceModel() = default;

    /// Load preference offsets from the local FeedbackManager database.
    void loadFromDatabase(FeedbackManager& fm) noexcept
    {
        // A failed read or a corrupt (non-finite) stored offset falls back to
        // "no learned offset" rather than whatever value() returns on error.
        auto load = [&fm](const char* key) {
            const float v = fm.getPreference(key, 0.0f).value_or(0.0f);
            return std::isfinite(v) ? v : 0.0f;
        };
        lufs_offset = load("pref_lufs_offset");
        lra_offset = load("pref_lra_offset");
        stereo_width_offset = load("pref_stereo_width_offset");
        low_end_weight_offset = load("pref_low_end_weight_offset");
    }

    /// Save current offsets to the database.
    void saveToDatabase(FeedbackManager& fm) const noexcept
    {
        fm.setPreference("pref_lufs_offset", lufs_offset);
        fm.setPreference("pref_lra_offset", lra_offset);
        fm.setPreference("pref_stereo_width_offset", stereo_width_offset);
        fm.setPreference("pref_low_end_weight_offset", low_end_weight_offset);
    }

    float getLufsOffset() const noexcept { return lufs_offset; }
    void setLufsOffset(float val) noexcept { lufs_offset = val; }

    float getLraOffset() const noexcept { return lra_offset; }
    void setLraOffset(float val) noexcept { lra_offset = val; }

    float getStereoWidthOffset() const noexcept { return stereo_width_offset; }
    void setStereoWidthOffset(float val) noexcept { stereo_width_offset = val; }

    float getLowEndWeightOffset() const noexcept { return low_end_weight_offset; }
    void setLowEndWeightOffset(float val) noexcept { low_end_weight_offset = val; }

    /// Adapt thresholds in a GenreEntry based on learned preferences.
    void adapt(GenreEntry& genre) const noexcept
    {
        // The safety clamps bound how far a LEARNED OFFSET may push a target;
        // they must never move a genre's own value (CORE-21: drone's -26 LUFS
        // minimum came back as -24 with a zero offset). Each bound is widened
        // to include the genre's original value.
        auto shift = [](float original, float offset, float lo, float hi) {
            return std::clamp(original + offset, std::min(lo, original), std::max(hi, original));
        };
        genre.lufs_target = shift(genre.lufs_target, lufs_offset, -24.0f, -6.0f);
        genre.lufs_range_min = shift(genre.lufs_range_min, lufs_offset, -24.0f, -8.0f);
        genre.lufs_range_max = shift(genre.lufs_range_max, lufs_offset, -20.0f, -4.0f);
        if (genre.lufs_range_min > genre.lufs_range_max) std::swap(genre.lufs_range_min, genre.lufs_range_max);

        genre.lra_range_min = std::max(std::min(1.0f, genre.lra_range_min), genre.lra_range_min + lra_offset);
        genre.lra_range_max = std::max(std::min(2.0f, genre.lra_range_max), genre.lra_range_max + lra_offset);
        if (genre.lra_range_min > genre.lra_range_max) std::swap(genre.lra_range_min, genre.lra_range_max);

        genre.stereo_width_min = shift(genre.stereo_width_min, stereo_width_offset, 0.1f, 0.9f);
        genre.stereo_width_max = shift(genre.stereo_width_max, stereo_width_offset, 0.2f, 1.0f);
        if (genre.stereo_width_min > genre.stereo_width_max) std::swap(genre.stereo_width_min, genre.stereo_width_max);

        genre.low_end_weight = std::clamp(genre.low_end_weight + low_end_weight_offset, 0.0f, 1.0f);
    }

    /// Update preference offsets by learning from user actions (accept/reject).
    /// Returns true if preference values changed.
    bool learnFromAction(const std::string& recType, bool accepted) noexcept
    {
        const float lr_lufs = 0.2f;
        const float lr_lra = 0.3f;
        const float lr_width = 0.05f;
        const float lr_bass = 0.05f;

        float prev_lufs = lufs_offset;
        float prev_lra = lra_offset;
        float prev_width = stereo_width_offset;
        float prev_bass = low_end_weight_offset;

        if (recType.find("lufs_too_loud") != std::string::npos) {
            if (!accepted)
                lufs_offset += lr_lufs; // rejected warning, they want it louder
            else
                lufs_offset = std::max(-3.0f, lufs_offset - lr_lufs * 0.2f);
        } else if (recType.find("lufs_too_quiet") != std::string::npos) {
            if (accepted)
                lufs_offset += lr_lufs; // accepted, want it louder
            else
                lufs_offset -= lr_lufs; // rejected, want it quieter
        } else if (recType.find("lra_too_compressed") != std::string::npos) {
            if (!accepted) lra_offset -= lr_lra; // rejected warning, prefer more compression
        } else if (recType.find("lra_too_dynamic") != std::string::npos) {
            if (!accepted) lra_offset += lr_lra; // rejected warning, prefer dynamic range
        } else if (recType.find("stereo_too_narrow") != std::string::npos) {
            if (!accepted) stereo_width_offset -= lr_width; // rejected narrow warning, like it narrower
        } else if (recType.find("stereo_too_wide") != std::string::npos) {
            if (!accepted) stereo_width_offset += lr_width; // rejected wide warning, like it wider
        } else if (recType.find("low_end_heavy") != std::string::npos) {
            if (!accepted) low_end_weight_offset += lr_bass; // rejected bass cut warning, like bass louder
        } else if (recType.find("low_end_light") != std::string::npos) {
            if (!accepted) low_end_weight_offset -= lr_bass; // rejected bass boost warning, like bass quieter
        }

        lufs_offset = std::clamp(lufs_offset, -4.0f, 4.0f);
        lra_offset = std::clamp(lra_offset, -6.0f, 6.0f);
        stereo_width_offset = std::clamp(stereo_width_offset, -0.4f, 0.4f);
        low_end_weight_offset = std::clamp(low_end_weight_offset, -0.3f, 0.3f);

        return prev_lufs != lufs_offset || prev_lra != lra_offset || prev_width != stereo_width_offset ||
               prev_bass != low_end_weight_offset;
    }

  private:
    float lufs_offset = 0.0f;
    float lra_offset = 0.0f;
    float stereo_width_offset = 0.0f;
    float low_end_weight_offset = 0.0f;
};

} // namespace khensu
