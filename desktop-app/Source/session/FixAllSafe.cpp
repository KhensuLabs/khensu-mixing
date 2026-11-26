// desktop-app/Source/session/FixAllSafe.cpp
#include "FixAllSafe.h"
#include <algorithm>
#include <cmath>
#include "RecommendationChain.h"

namespace khensu::fix_all_safe {

namespace {
bool needsListening(const Chain& chain)
{
    // RUN-6: a denoise of a bounced master takes music with the noise.
    return std::any_of(chain.begin(), chain.end(),
                       [](const ipc::HttpServiceBridge::EffectSpec& e) { return e.effect == "SpectralDenoise"; });
}

float param(const ipc::HttpServiceBridge::EffectSpec& e, const char* name, float fallback)
{
    const auto it = e.parameters.find(name);
    return it != e.parameters.end() ? it->second : fallback;
}
} // namespace

int stageRank(const juce::String& effect)
{
    if (effect == "HighpassFilter") return 0;
    if (effect == "LowShelfFilter" || effect == "HighShelfFilter" || effect == "PeakFilter" ||
        effect == "LowpassFilter")
        return 1;
    if (effect == "SpectralDenoise") return 2;
    if (effect == "Compressor" || effect == "DeEsser" || effect == "TransientShaper") return 3;
    if (effect == "TapeSaturation") return 4;
    if (effect == "StereoWidener") return 5;
    if (effect == "Gain") return 6;
    if (effect == "Limiter") return 7;
    return 5; // anything else: before the level stages
}

juce::String whyNotSafe(const IIssues::Issue& issue, const Evidence& evidence)
{
    if (!issue.topFixCandidate()) {
        if (issue.settled()) return TRANS("already settled");
        if (issue.code == "noise_floor_high") return TRANS("a denoise needs listening - fix it on its own");
        return TRANS("advice only - no one-click fix");
    }
    const auto it = evidence.find(juce::String(issue.code));
    if (it == evidence.end()) return TRANS("its fix was never measured on real music");
    const auto& e = it->second;
    if (e.adviceOnly) return TRANS("advice only - the fix is made at the source");
    if (e.fixTried < kMinTried || e.fixClearsRate < 0.0) return TRANS("its fix was measured on too few tracks");
    if (e.fixClearsRate < kMinClearsRate)
        return TRANS("its fix cleared it on only %r% of tracks")
            .replace("%r", juce::String(juce::roundToInt(e.fixClearsRate * 100.0)));
    if (needsListening(buildRecommendationChain(issue.rec)))
        return TRANS("a denoise needs listening - fix it on its own");
    return {};
}

Chain mergeChains(const std::vector<Chain>& chains)
{
    Chain stages;
    bool haveHighpass = false, haveGain = false, haveLimiter = false;
    ipc::HttpServiceBridge::EffectSpec highpass, gain, limiter;
    for (const auto& chain : chains)
        for (const auto& e : chain) {
            if (e.effect == "HighpassFilter") {
                const float cutoff = param(e, "cutoff_frequency_hz", 0.0f);
                if (!haveHighpass || cutoff > param(highpass, "cutoff_frequency_hz", 0.0f)) highpass = e;
                haveHighpass = true;
            } else if (e.effect == "Gain") {
                if (!haveGain) {
                    gain = e;
                    gain.parameters["gain_db"] = 0.0f;
                }
                gain.parameters["gain_db"] += param(e, "gain_db", 0.0f);
                haveGain = true;
            } else if (e.effect == "Limiter") {
                const float ceiling = param(e, "threshold_db", 0.0f);
                if (!haveLimiter || ceiling < param(limiter, "threshold_db", 0.0f)) limiter = e;
                haveLimiter = true;
            } else {
                stages.push_back(e);
            }
        }
    if (haveHighpass) stages.push_back(highpass);
    if (haveGain && std::abs(param(gain, "gain_db", 0.0f)) >= 0.05f) stages.push_back(gain);
    if (haveLimiter) stages.push_back(limiter);
    std::stable_sort(stages.begin(), stages.end(),
                     [](const ipc::HttpServiceBridge::EffectSpec& a, const ipc::HttpServiceBridge::EffectSpec& b) {
                         return stageRank(a.effect) < stageRank(b.effect);
                     });
    return stages;
}

Plan plan(const std::vector<IIssues::Issue>& issues, const Evidence& evidence)
{
    Plan out;
    std::vector<Chain> chains;
    for (const auto& issue : issues) {
        if (issue.settled() || issue.recId.empty()) continue;
        if (const auto why = whyNotSafe(issue, evidence); why.isNotEmpty()) {
            out.skipped.emplace_back(issue.code, why);
            continue;
        }
        auto chain = buildRecommendationChain(issue.rec);
        if (chain.empty()) {
            out.skipped.emplace_back(issue.code, TRANS("advice only - no one-click fix"));
            continue;
        }
        out.recIds.push_back(issue.recId);
        out.codes.push_back(issue.code);
        chains.push_back(std::move(chain));
    }
    out.chain = mergeChains(chains);
    return out;
}

} // namespace khensu::fix_all_safe
