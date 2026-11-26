// shared-core/include/khensu/recommendations/MaskingChain.h
#pragma once
// ENH-6: a stem-masking finding's fix as a KEYED dynamic EQ - the band ducks
// in the competing stem only while the leading stem (the key) is loud there,
// which is what templates_masking.py advises ("a sidechained / vocal-keyed
// dynamic EQ"), instead of a static cut that thins the part all the time.
//
// MaskingRecommender writes the plan into the finding's suggested_parameters
// (floats only, so the stems travel as indices into kMaskingStems):
//   dyneq_freq_hz, dyneq_q, dyneq_threshold_db, dyneq_max_cut_db,
//   dyneq_key_stem, dyneq_target_stem
// maskingDynamicEq() turns them into the DynamicEQ chain stage. The stage
// keys on the render's key signal (OfflineRenderOptions::keyPath = the key
// stem's file) and is meant for the target stem; rendered without a key it
// keys on its own input (and the service, which has no key, does the same).
#include <khensu/dsp/ChainProcessor.h>
#include <khensu/recommendations/Recommendation.h>

#include <algorithm>
#include <array>
#include <map>
#include <cmath>
#include <optional>
#include <string>

namespace khensu {

/// The separated stems a masking finding names, in priority order: the
/// earlier stem of a pair leads (keys the cut), the later one makes room.
inline constexpr std::array<const char*, 4> kMaskingStems = {"vocals", "drums", "bass", "other"};

/// Index of `stem` in kMaskingStems, -1 when it is not one of them.
inline int maskingStemIndex(const std::string& stem) noexcept
{
    for (size_t i = 0; i < kMaskingStems.size(); ++i)
        if (stem == kMaskingStems[i]) return static_cast<int>(i);
    return -1;
}

/// The stem name for a stored index, empty when out of range.
inline std::string maskingStemName(float index)
{
    const long i = std::lround(index);
    return i >= 0 && i < static_cast<long>(kMaskingStems.size()) ? std::string(kMaskingStems[static_cast<size_t>(i)])
                                                                 : std::string();
}

/// The dynamic EQ the finding asks for, and which stems it involves.
struct MaskingDynamicEq {
    std::string keyStem;    ///< the stem that leads (detector); empty: self-keyed
    std::string targetStem; ///< the stem the cut is for; empty when unknown
    dsp::ChainStageSpec stage;
};

/// Writes the plan for the band [loHz, hiHz] of the pair (a, b) into `params`
/// (cut depth `cutDb`, negative). The higher-priority stem keys the other.
inline void writeMaskingDynamicEq(std::map<std::string, float>& params, const std::string& a, const std::string& b,
                                  float loHz, float hiHz, float cutDb)
{
    if (!(loHz > 0.0f) || !(hiHz > loHz)) return;
    const int ia = maskingStemIndex(a), ib = maskingStemIndex(b);
    // Lower index leads; an unknown stem never leads a known one.
    const bool aLeads = ia >= 0 && (ib < 0 || ia <= ib);
    const int key = aLeads ? ia : ib;
    const int target = aLeads ? ib : ia;
    // The DynamicEQ band is 1/q octaves wide around its (geometric) centre.
    const float octaves = std::log2(hiHz / loHz);
    params["dyneq_freq_hz"] = std::sqrt(loHz * hiHz);
    params["dyneq_q"] = std::clamp(octaves > 0.0f ? 1.0f / octaves : 2.0f, 0.3f, 18.0f);
    params["dyneq_threshold_db"] = -30.0f;
    params["dyneq_max_cut_db"] = std::abs(cutDb);
    if (key >= 0) params["dyneq_key_stem"] = static_cast<float>(key);
    if (target >= 0) params["dyneq_target_stem"] = static_cast<float>(target);
}

/// The DynamicEQ stage `rec` suggests (nullopt when it suggests none).
inline std::optional<MaskingDynamicEq> maskingDynamicEq(const Recommendation& rec)
{
    const auto& p = rec.suggested_parameters;
    const auto get = [&p](const char* k, float fallback) {
        const auto it = p.find(k);
        return it != p.end() ? it->second : fallback;
    };
    if (p.count("dyneq_freq_hz") == 0) return std::nullopt;
    MaskingDynamicEq out;
    if (p.count("dyneq_key_stem") != 0) out.keyStem = maskingStemName(p.at("dyneq_key_stem"));
    if (p.count("dyneq_target_stem") != 0) out.targetStem = maskingStemName(p.at("dyneq_target_stem"));
    out.stage.effect = "DynamicEQ";
    out.stage.parameters = {
        {"frequency_hz", get("dyneq_freq_hz", 1000.0f)},
        {"q", get("dyneq_q", 2.0f)},
        {"threshold_db", get("dyneq_threshold_db", -30.0f)},
        {"ratio", 4.0f},
        {"attack_ms", 5.0f},
        {"release_ms", 120.0f},
        {"max_reduction_db", std::abs(get("dyneq_max_cut_db", 4.0f))},
        {"use_key", out.keyStem.empty() ? 0.0f : 1.0f},
    };
    return out;
}

} // namespace khensu
