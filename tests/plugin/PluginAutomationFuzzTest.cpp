// tests/plugin/PluginAutomationFuzzTest.cpp
// Reproduces pluginval's strictness-10 "Automation" test: every parameter set
// to a random normalized value between 32-sample sub-blocks, at 44.1/48/96 kHz
// and several host block sizes. The output must stay finite - NaNs here are
// the failure pluginval reported at 96 kHz.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include "../../plugin-copilot/Source/PluginProcessor.h"

#include <cmath>

using khensu::KhensuCopilotProcessor;

namespace {
int countNonFinite(const juce::AudioBuffer<float>& b)
{
    int n = 0;
    for (int c = 0; c < b.getNumChannels(); ++c)
        for (int i = 0; i < b.getNumSamples(); ++i)
            if (!std::isfinite(b.getSample(c, i))) ++n;
    return n;
}
} // namespace

TEST_CASE("Random per-sub-block parameter automation never produces NaN", "[plugin][automation][pluginval]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    // ONE instance across every rate/block size, release+prepare in between,
    // exactly as pluginval drives it - state carries over between configs.
    KhensuCopilotProcessor proc;
    juce::Random rng(1234);
    for (double sr : {44100.0, 48000.0, 96000.0}) {
        for (int block : {64, 128, 256, 512, 1024}) {
            proc.releaseResources();
            proc.setPlayConfigDetails(2, 2, sr, block);
            proc.prepareToPlay(sr, block);
            auto& params = proc.getParameters();
            juce::AudioBuffer<float> buffer(2, block);
            juce::MidiBuffer midi;
            int bad = 0;
            for (int iteration = 0; iteration < 400 && bad == 0; ++iteration) {
                for (int c = 0; c < 2; ++c)
                    for (int i = 0; i < block; ++i) buffer.setSample(c, i, rng.nextFloat() * 2.0f - 1.0f);
                // pluginval sets every parameter, then processes sub-blocks of 32.
                for (auto* p : params) p->setValue(rng.nextFloat());
                for (int start = 0; start < block; start += 32) {
                    const int len = juce::jmin(32, block - start);
                    juce::AudioBuffer<float> sub(buffer.getArrayOfWritePointers(), 2, start, len);
                    proc.processBlock(sub, midi);
                }
                bad = countNonFinite(buffer);
                // The corrective-DSP control worker publishes on its own thread;
                // give it a beat like a host's real-time cadence would.
                if (iteration % 10 == 0) juce::Thread::sleep(6);
            }
            INFO("sample rate " << sr << ", block " << block);
            CHECK(bad == 0);
        }
    }
}

TEST_CASE("Multiband path gives identical output for sub-blocked and whole host blocks", "[plugin][automation][pluginval]")
{
    // Regression for the NaN pluginval found: band buffers are prepared at the
    // maximum block size, and a short host callback used to run the crossover
    // and band compressors over the stale tail too. Splitting a block into
    // 32-sample callbacks must now be bit-for-bit the same as one callback.
    juce::ScopedJuceInitialiser_GUI juceInit;
    constexpr double sr = 96000.0;
    constexpr int block = 1024;
    const bool multiband = GENERATE(true, false);
    INFO("multiband " << multiband);
    auto configure = [multiband](KhensuCopilotProcessor& p) {
        // Parameters BEFORE prepare: smoothers start at their targets, so no
        // parameter ramp (which is legitimately block-rate) muddies the check.
        p.setParamThreadSafe(KhensuCopilotProcessor::ParamIDs::compOn, 1.0f);
        p.setParamThreadSafe(KhensuCopilotProcessor::ParamIDs::compMultiband, multiband ? 1.0f : 0.0f);
        p.setParamThreadSafe(KhensuCopilotProcessor::ParamIDs::compLowThreshDb, -30.0f);
        p.setParamThreadSafe(KhensuCopilotProcessor::ParamIDs::compHighThreshDb, -30.0f);
        p.setPlayConfigDetails(2, 2, sr, block);
        p.prepareToPlay(sr, block);
    };
    KhensuCopilotProcessor whole, split;
    configure(whole);
    configure(split);

    juce::MidiBuffer midi;
    auto signal = [](int offset, int n) {
        juce::AudioBuffer<float> b(2, n);
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < n; ++i)
                b.setSample(c, i, 0.6f * std::sin(0.031f * static_cast<float>(offset + i)) +
                                      0.3f * std::sin(0.47f * static_cast<float>(offset + i)));
        return b;
    };
    double maxDiff = 0.0;
    int firstDiff = -1;
    for (int rep = 0; rep < 8; ++rep) {
        auto a = signal(rep * block, block);
        auto b = signal(rep * block, block);
        whole.processBlock(a, midi);
        for (int start = 0; start < block; start += 32) {
            juce::AudioBuffer<float> sub(b.getArrayOfWritePointers(), 2, start, 32);
            split.processBlock(sub, midi);
        }
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < block; ++i) {
                REQUIRE(std::isfinite(b.getSample(c, i)));
                const double d = std::abs(a.getSample(c, i) - b.getSample(c, i));
                if (d > 1.0e-3 && firstDiff < 0) firstDiff = rep * block + i;
                maxDiff = std::max(maxDiff, d);
            }
    }
    UNSCOPED_INFO("maxDiff " << maxDiff << " first diverging sample " << firstDiff);
    CHECK(maxDiff < 1.0e-3);
}
