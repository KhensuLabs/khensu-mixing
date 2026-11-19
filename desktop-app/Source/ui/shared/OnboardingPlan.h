// desktop-app/Source/ui/shared/OnboardingPlan.h
// Onboarding wizard logic: goal -> target -> suggested starting chain.
// Turns the user's goal, genre (GenreDatabase entry) and delivery platform
// into concrete loudness/peak targets plus an ordered processing chain with
// built-in-DSP parameter values (DspParamIds) ready for pushPluginParams.
// Pure and deterministic - unit-tested in tests/ui/OnboardingPlanTest.cpp.
#pragma once
#include <JuceHeader.h>
#include <khensu/mixerchain/DspChainMapper.h>
#include <khensu/targets/GenreEntry.h>
#include <map>
#include <vector>

namespace khensu {

struct OnboardingPlan {
    enum class Goal { MixCheck, MasterForRelease, VocalPolish, MatchReference };
    enum class Platform { Streaming, AppleMusic, YouTube, Club, Broadcast };

    struct Stage {
        juce::String name;
        juce::String why;
        std::map<juce::String, float> params; ///< DspParamIds -> value (may be empty)
    };

    Goal goal = Goal::MixCheck;
    Platform platform = Platform::Streaming;
    juce::String genreId, genreName;
    float targetLufs = -14.0f;
    float truePeakCeiling = -1.0f;
    std::vector<Stage> chain;

    /// All stage params merged in chain order (later stages win on a clash).
    std::map<juce::String, float> combinedParams() const
    {
        std::map<juce::String, float> out;
        for (const auto& s : chain)
            for (const auto& [k, v] : s.params)
                out[k] = v;
        return out;
    }

    juce::String summary() const
    {
        juce::String s;
        s << goalName(goal) << " - " << (genreName.isNotEmpty() ? genreName : juce::String("any genre")) << " for "
          << platformName(platform) << ": aim for " << juce::String(targetLufs, 1) << " LUFS, peaks at or below "
          << juce::String(truePeakCeiling, 1) << " dBTP.";
        return s;
    }

    static juce::String goalName(Goal g)
    {
        switch (g) {
        case Goal::MixCheck:
            return "Check my mix";
        case Goal::MasterForRelease:
            return "Master for release";
        case Goal::VocalPolish:
            return "Polish the vocal";
        case Goal::MatchReference:
            return "Match a reference";
        }
        return {};
    }

    /// WF-6: the export target (analysis/delivery_targets.py id) a finished
    /// onboarding pre-selects on Export; empty for Club (no normalisation).
    static juce::String exportTargetIdForPlatformName(const juce::String& name)
    {
        if (name == platformName(Platform::Streaming)) return "spotify";
        if (name == platformName(Platform::AppleMusic)) return "apple_music";
        if (name == platformName(Platform::YouTube)) return "youtube";
        if (name == platformName(Platform::Broadcast)) return "ebu_r128";
        return {};
    }

    static juce::String platformName(Platform p)
    {
        switch (p) {
        case Platform::Streaming:
            return "Spotify / streaming";
        case Platform::AppleMusic:
            return "Apple Music";
        case Platform::YouTube:
            return "YouTube";
        case Platform::Club:
            return "Club / DJ";
        case Platform::Broadcast:
            return "Broadcast (EBU R128)";
        }
        return {};
    }

    /// Platform loudness normalization target and true-peak ceiling. Club
    /// has no normalization, so the genre's own (louder) target is used.
    static std::pair<float, float> platformTargets(Platform p, const GenreEntry& genre)
    {
        switch (p) {
        case Platform::Streaming:
            return {-14.0f, -1.0f};
        case Platform::AppleMusic:
            return {-16.0f, -1.0f};
        case Platform::YouTube:
            return {-14.0f, -1.0f};
        case Platform::Broadcast:
            return {-23.0f, -1.0f};
        case Platform::Club:
            return {juce::jmin(-6.0f, genre.lufs_target), juce::jmin(-0.3f, genre.true_peak_max)};
        }
        return {-14.0f, -1.0f};
    }

    static OnboardingPlan build(Goal goal, const GenreEntry& genre, Platform platform)
    {
        using P = mixerchain::DspParamIds;
        OnboardingPlan plan;
        plan.goal = goal;
        plan.platform = platform;
        plan.genreId = juce::String(genre.id);
        plan.genreName = juce::String(genre.name);
        const auto [lufs, tp] = platformTargets(platform, genre);
        plan.targetLufs = lufs;
        plan.truePeakCeiling = tp;

        const float lowWeight = juce::jlimit(0.0f, 1.0f, genre.low_end_weight);
        const float ratio = juce::jlimit(1.2f, 8.0f, genre.compression_ratio_typical);
        // Bass-forward genres keep more sub: a lower high-pass.
        const float hpf = juce::jmap(lowWeight, 45.0f, 20.0f);
        // Darker target tilts get a gentle air lift less often; brighter ones more.
        const float airGain = juce::jlimit(0.0f, 2.5f, (genre.spectral_tilt_target_db_per_octave + 6.0f) * 0.8f);

        auto stage = [&plan](juce::String name, juce::String why, std::map<juce::String, float> params) {
            plan.chain.push_back({std::move(name), std::move(why), std::move(params)});
        };

        switch (goal) {
        case Goal::MixCheck:
            stage("Analyze first",
                  "Run intake: the Dashboard flags masking, balance and translation issues "
                  "against the " +
                      plan.genreName + " targets before you touch a fader.",
                  {});
            stage("Clean the low end",
                  "High-pass rumble below " + juce::String(juce::roundToInt(hpf)) +
                      " Hz so the low end the genre needs stays clear.",
                  {{P::hpfOn, 1.0f}, {P::hpfFreq, hpf}});
            stage("Gentle mix-bus glue",
                  "A slow " + juce::String(ratio * 0.5f, 1) + ":1 bus compressor to audition glue without squashing.",
                  {{P::compOn, 1.0f},
                   {P::compRatio, juce::jmax(1.5f, ratio * 0.5f)},
                   {P::compThreshDb, -18.0f},
                   {P::compAttackMs, 30.0f},
                   {P::compReleaseMs, 200.0f}});
            break;
        case Goal::MasterForRelease:
            stage("Corrective low cut", "Remove inaudible sub energy that eats limiter headroom.",
                  {{P::hpfOn, 1.0f}, {P::hpfFreq, juce::jmax(20.0f, hpf - 10.0f)}});
            stage("Tonal shaping", "A broad air shelf toward the genre's tonal tilt.",
                  {{P::eqOn, 1.0f}, {P::eqFreq, 10000.0f}, {P::eqGainDb, airGain}, {P::eqQ, 0.7f}});
            stage("Glue compression",
                  juce::String(ratio, 1) + ":1, slow attack - keeps transients, "
                                           "controls the body.",
                  {{P::compOn, 1.0f},
                   {P::compRatio, ratio},
                   {P::compThreshDb, -20.0f},
                   {P::compAttackMs, 30.0f},
                   {P::compReleaseMs, 150.0f}});
            stage("Limiter to target",
                  "Ceiling " + juce::String(tp, 1) +
                      " dBTP; raise input until the "
                      "meter reads " +
                      juce::String(lufs, 1) + " LUFS integrated.",
                  {{P::limOn, 1.0f}, {P::limThreshDb, tp}});
            break;
        case Goal::VocalPolish:
            stage("Vocal high-pass", "Cut plosive and room rumble below 90 Hz.",
                  {{P::hpfOn, 1.0f}, {P::hpfFreq, 90.0f}});
            stage("Presence",
                  "Lift 3 kHz so words read " +
                      juce::String(genre.vocal_presence_db >= 0.0f ? "forward" : "inside the mix") +
                      ", as the genre expects.",
                  {{P::eqOn, 1.0f},
                   {P::eqFreq, 3000.0f},
                   {P::eqGainDb, juce::jlimit(0.5f, 3.0f, 1.5f + genre.vocal_presence_db * 0.5f)},
                   {P::eqQ, 1.0f}});
            stage("Level, then compress",
                  "Ride phrases with Smart Gain Rider, then 3:1 with a fast-ish "
                  "attack for consistency.",
                  {{P::compOn, 1.0f},
                   {P::compRatio, 3.0f},
                   {P::compThreshDb, -20.0f},
                   {P::compAttackMs, 8.0f},
                   {P::compReleaseMs, 90.0f}});
            break;
        case Goal::MatchReference:
            stage("Load a reference",
                  "Add a commercial " + plan.genreName +
                      " track in the Reference "
                      "Library; the Reference Compare panel shows the tonal delta.",
                  {});
            stage("Match the tilt", "Start from a broad shelf toward the reference curve.",
                  {{P::eqOn, 1.0f}, {P::eqFreq, 8000.0f}, {P::eqGainDb, airGain}, {P::eqQ, 0.6f}});
            stage("Match the loudness", "Limit to the reference's level for a fair, gain-matched A/B.",
                  {{P::limOn, 1.0f}, {P::limThreshDb, tp}});
            break;
        }
        return plan;
    }
};

} // namespace khensu
