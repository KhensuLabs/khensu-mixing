// desktop-app/Source/ui/ExportDelivery/MasterMacros.h
// FEAT-2: the Master Assistant's five macros over the mastering agent's
// chain - Loudness (gain into the limiter), Tone (how much of the chain's EQ
// moves), Width, Punch (how hard the compression and transient stages work)
// and Ceiling (the limiter's true-peak ceiling). Neutral macros return the
// chain unchanged except the ceiling, which every master sets.
// THREAD_CONTEXT(AnyThread) - pure.
#pragma once
#include <khensu/ipc/HttpServiceBridge.h>
#include <algorithm>
#include <vector>

namespace khensu::master_macros {

using Effect = ipc::HttpServiceBridge::EffectSpec;
using Chain = std::vector<Effect>;

struct Macros {
    float loudnessDb = 0.0f;   ///< -6..+6: gain into the limiter
    float tone = 1.0f;         ///< 0..1.5: scales every EQ stage's gain
    float widthPct = 0.0f;     ///< -50..+50: the stereo width, relative
    float punch = 1.0f;        ///< 0..1.5: scales compression ratio above 1:1 and transient attack
    float ceilingDbtp = -1.0f; ///< -3..-0.1: the limiter's true-peak ceiling

    bool operator==(const Macros& o) const noexcept
    {
        return loudnessDb == o.loudnessDb && tone == o.tone && widthPct == o.widthPct && punch == o.punch &&
               ceilingDbtp == o.ceilingDbtp;
    }
};

inline bool isEq(const juce::String& e) { return e == "PeakFilter" || e == "HighShelfFilter" || e == "LowShelfFilter"; }

/// The chain with the macros applied (see the file comment).
inline Chain apply(const Chain& base, const Macros& m)
{
    Chain out;
    out.reserve(base.size() + 3);
    for (auto fx : base) {
        auto& p = fx.parameters;
        if (isEq(fx.effect) && p.count("gain_db") != 0) {
            p["gain_db"] = std::clamp(p["gain_db"] * m.tone, -24.0f, 24.0f);
        } else if ((fx.effect == "Compressor" || fx.effect == "GlueCompressor") && p.count("ratio") != 0) {
            const float maxRatio = fx.effect == "Compressor" ? 30.0f : 20.0f;
            p["ratio"] = std::clamp(1.0f + (p["ratio"] - 1.0f) * m.punch, 1.0f, maxRatio);
        } else if (fx.effect == "TransientShaper" && p.count("attack_gain_db") != 0) {
            p["attack_gain_db"] = std::clamp(p["attack_gain_db"] * m.punch, -24.0f, 24.0f);
        } else if (fx.effect == "StereoWidener") {
            const float w = p.count("width") != 0 ? p["width"] : 1.0f;
            p["width"] = std::clamp(w * (1.0f + m.widthPct / 100.0f), 0.0f, 2.0f);
        }
        out.push_back(std::move(fx));
    }

    // Width with no widener in the chain: one before the limiter.
    const bool hasWidener =
        std::any_of(out.begin(), out.end(), [](const Effect& e) { return e.effect == "StereoWidener"; });
    auto limiterAt = [&out] {
        return std::find_if(out.begin(), out.end(), [](const Effect& e) { return e.effect == "Limiter"; });
    };
    if (!hasWidener && m.widthPct != 0.0f)
        out.insert(limiterAt(),
                   Effect{"StereoWidener", {{"width", std::clamp(1.0f + m.widthPct / 100.0f, 0.0f, 2.0f)}}});

    // Every master ends in the limiter at the ceiling.
    auto lim = limiterAt();
    if (lim == out.end()) {
        out.push_back(Effect{"Limiter", {{"threshold_db", 0.0f}, {"release_ms", 100.0f}}});
        lim = std::prev(out.end());
    }
    lim->parameters["threshold_db"] = std::clamp(m.ceilingDbtp, -30.0f, 0.0f);

    // Loudness: gain into the limiter (a Gain stage just before it).
    if (m.loudnessDb != 0.0f) {
        if (lim != out.begin() && std::prev(lim)->effect == "Gain") {
            auto& g = std::prev(lim)->parameters["gain_db"];
            g = std::clamp(g + m.loudnessDb, -36.0f, 24.0f);
        } else {
            out.insert(lim, Effect{"Gain", {{"gain_db", std::clamp(m.loudnessDb, -36.0f, 24.0f)}}});
        }
    }
    return out;
}

} // namespace khensu::master_macros
