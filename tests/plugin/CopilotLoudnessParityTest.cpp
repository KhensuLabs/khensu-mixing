// FIX-11: the plugin's live loudness and the desktop analyzer must read the
// same audio the same. Both K-weight with khensu::analysis::kWeightingCoefficients
// (shared-core) and sum 100 ms bins, so on identical input they may differ only
// by the float the reading is published in.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "../../plugin-copilot/Source/CopilotRealtimeTelemetry.h"

#include <khensu/analysis/LoudnessAnalyzer.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

using Catch::Matchers::WithinAbs;

namespace {

/// Same filter, same bins, double precision on both sides: what is left is the
/// rounding of a float reading (about 2e-6 LU at -20 LUFS). 0.005 LU leaves
/// room for that and is 20x tighter than the 0.1 LU a meter displays.
constexpr double kToleranceLu = 0.005;
constexpr double kTwoPi = 6.283185307179586476925286766559;

enum class Signal { Sine997, PinkNoise, LowHeavy };

const char* name(Signal s)
{
    switch (s) {
    case Signal::Sine997: return "997 Hz sine at -18 dBFS";
    case Signal::PinkNoise: return "pink noise";
    case Signal::LowHeavy:
    default: return "low-heavy (45 + 70 + 180 Hz)";
    }
}

/// Deterministic test audio; channel 1 is not a copy of channel 0, so the
/// channel sum is exercised too.
std::vector<float> makeChannel(Signal signal, double fs, int channel, int numSamples)
{
    std::vector<float> x(static_cast<size_t>(numSamples));
    if (signal == Signal::Sine997) {
        const double amp = std::pow(10.0, -18.0 / 20.0);
        for (int i = 0; i < numSamples; ++i)
            x[static_cast<size_t>(i)] = static_cast<float>(amp * std::sin(kTwoPi * 997.0 * i / fs + 0.4 * channel));
    } else if (signal == Signal::PinkNoise) {
        // White noise from an LCG through Paul Kellet's economy pinking filter.
        std::uint32_t state = channel == 0 ? 0x1234567u : 0x89abcdeu;
        double b0 = 0.0, b1 = 0.0, b2 = 0.0;
        for (int i = 0; i < numSamples; ++i) {
            state = state * 1664525u + 1013904223u;
            const double white = static_cast<double>(state >> 8) / 8388608.0 - 1.0;
            b0 = 0.99765 * b0 + white * 0.0990460;
            b1 = 0.96300 * b1 + white * 0.2965164;
            b2 = 0.57000 * b2 + white * 1.0526913;
            x[static_cast<size_t>(i)] = static_cast<float>(0.05 * (b0 + b1 + b2 + white * 0.1848));
        }
    } else {
        const double gain = channel == 0 ? 1.0 : 0.8;
        for (int i = 0; i < numSamples; ++i) {
            const double t = i / fs;
            x[static_cast<size_t>(i)] =
                static_cast<float>(gain * (0.25 * std::sin(kTwoPi * 45.0 * t) + 0.12 * std::sin(kTwoPi * 70.0 * t + 1.0)
                                           + 0.02 * std::sin(kTwoPi * 180.0 * t + 0.3 * channel)));
        }
    }
    return x;
}

struct Readings {
    float pluginMomentary = 0.0f;  ///< after the first 400 ms
    float pluginShortTerm = 0.0f;  ///< after 3 s
    float desktopMomentary = 0.0f; ///< LoudnessAnalyzer over the same 400 ms
    float desktopShortTerm = 0.0f; ///< LoudnessAnalyzer over the same 3 s
};

Readings measure(Signal signal, double fs, int numChannels)
{
    const int bin = static_cast<int>(fs * 0.1 + 0.5);
    const int momentarySamples = 4 * bin;
    const int shortTermSamples = 30 * bin;
    std::vector<std::vector<float>> audio;
    for (int c = 0; c < numChannels; ++c) audio.push_back(makeChannel(signal, fs, c, shortTermSamples));

    Readings r;

    // The plugin's audio-thread path, in host-sized blocks that do not line up
    // with the 100 ms bins.
    khensu::CopilotRealtimeTelemetry telemetry;
    telemetry.prepareToPlay(fs, numChannels);
    constexpr int kHostBlock = 512;
    bool momentaryTaken = false;
    for (int start = 0; start < shortTermSamples; start += kHostBlock) {
        const int n = std::min(kHostBlock, shortTermSamples - start);
        juce::AudioBuffer<float> block(numChannels, n);
        for (int c = 0; c < numChannels; ++c)
            block.copyFrom(c, 0, audio[static_cast<size_t>(c)].data() + start, n);
        telemetry.process(block);
        // 4 bins are complete and the 5th is still open (a host block is
        // shorter than a bin), so momentary covers exactly the first 400 ms.
        if (!momentaryTaken && start + n >= momentarySamples) {
            r.pluginMomentary = telemetry.snapshot().momentary_lufs;
            momentaryTaken = true;
        }
    }
    r.pluginShortTerm = telemetry.snapshot().short_term_lufs;

    // The desktop analyzer on the same samples. Four blocks make one 400 ms
    // window (which is then the gated integrated value); thirty make one 3 s
    // short-term window.
    std::vector<const float*> channels;
    for (const auto& ch : audio) channels.push_back(ch.data());
    khensu::LoudnessAnalyzer momentary;
    momentary.reset(fs, numChannels);
    momentary.process(channels.data(), numChannels, momentarySamples);
    r.desktopMomentary = momentary.getResult().lufs_integrated;
    khensu::LoudnessAnalyzer shortTerm;
    shortTerm.reset(fs, numChannels);
    shortTerm.process(channels.data(), numChannels, shortTermSamples);
    r.desktopShortTerm = shortTerm.getResult().lufs_short_term_max;
    return r;
}

} // namespace

TEST_CASE("FIX-11: plugin live loudness agrees with the desktop analyzer", "[plugin][fix11][loudness]")
{
    for (const double fs : {44100.0, 48000.0}) {
        for (const Signal signal : {Signal::Sine997, Signal::PinkNoise, Signal::LowHeavy}) {
            for (const int numChannels : {1, 2}) {
                const Readings r = measure(signal, fs, numChannels);
                INFO(name(signal) << ", " << fs << " Hz, " << numChannels << " ch: short-term plugin "
                                  << r.pluginShortTerm << " desktop " << r.desktopShortTerm << " (diff "
                                  << (r.pluginShortTerm - r.desktopShortTerm) << " LU); momentary plugin "
                                  << r.pluginMomentary << " desktop " << r.desktopMomentary << " (diff "
                                  << (r.pluginMomentary - r.desktopMomentary) << " LU)");
                // A real reading, not two meters agreeing on their floor.
                CHECK(r.desktopShortTerm > -60.0f);
                CHECK_THAT(r.pluginShortTerm, WithinAbs(r.desktopShortTerm, kToleranceLu));
                CHECK_THAT(r.pluginMomentary, WithinAbs(r.desktopMomentary, kToleranceLu));
            }
        }
    }
}

TEST_CASE("FIX-11: plugin live loudness of a 997 Hz sine matches the BS.1770 reference", "[plugin][fix11][loudness]")
{
    // A 997 Hz sine at -18 dBFS peak in one channel reads -21.0 LUFS (the
    // -3.01 dB RMS and K-weighting's +0.69 dB at 997 Hz cancel the -0.691).
    for (const double fs : {44100.0, 48000.0}) {
        const Readings r = measure(Signal::Sine997, fs, 1);
        INFO(fs << " Hz: plugin short-term " << r.pluginShortTerm);
        CHECK_THAT(r.pluginShortTerm, WithinAbs(-21.0, 0.05));
    }
}
