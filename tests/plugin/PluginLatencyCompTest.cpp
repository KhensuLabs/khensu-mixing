// tests/plugin/PluginLatencyCompTest.cpp
// Latency-compensation correctness for the hosted-plugin chain (audit CQ-02):
// Delta must null against a latency-bearing hosted plugin, Original must keep
// wet-path timing, partial-wet must not comb-filter, and the reported host
// latency must match the path the signal actually takes.
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "../../plugin-copilot/Source/PluginProcessor.h"
#include "FakeHostedInstances.h"

using khensu::KhensuCopilotProcessor;
using khensu::test::FakeLatencyInstance;

namespace {

constexpr double kSr = 48000.0;
constexpr int kBlock = 512;

struct ProcessorFixture {
    juce::ScopedJuceInitialiser_GUI juceInit;
    KhensuCopilotProcessor proc;

    ProcessorFixture()
    {
        proc.setPlayConfigDetails(2, 2, kSr, kBlock);
        proc.prepareToPlay(kSr, kBlock);
    }

    void addFakeLatencyPlugin(int latencySamples)
    {
        auto fake = std::make_unique<FakeLatencyInstance>(latencySamples);
        fake->prepareToPlay(kSr, kBlock);
        proc.addPluginInstanceToChain(std::move(fake), "FakeLatency");
    }

    /// Runs `blocks` blocks of silence through the processor (state priming).
    void prime(int blocks)
    {
        juce::AudioBuffer<float> buf(2, kBlock);
        juce::MidiBuffer midi;
        for (int b = 0; b < blocks; ++b) {
            buf.clear();
            proc.processBlock(buf, midi);
        }
    }

    /// Feeds an impulse block followed by silence; returns the concatenated
    /// output of `blocks` blocks.
    juce::AudioBuffer<float> impulseResponse(int blocks)
    {
        juce::AudioBuffer<float> out(2, kBlock * blocks);
        juce::AudioBuffer<float> buf(2, kBlock);
        juce::MidiBuffer midi;
        for (int b = 0; b < blocks; ++b) {
            buf.clear();
            if (b == 0) buf.setSample(0, 0, 1.0f), buf.setSample(1, 0, 1.0f);
            proc.processBlock(buf, midi);
            for (int c = 0; c < 2; ++c)
                out.copyFrom(c, b * kBlock, buf, c, 0, kBlock);
        }
        return out;
    }
};

int firstPeakIndex(const juce::AudioBuffer<float>& b, float thresh = 0.05f)
{
    for (int i = 0; i < b.getNumSamples(); ++i)
        if (std::abs(b.getSample(0, i)) > thresh) return i;
    return -1;
}

int countPeaks(const juce::AudioBuffer<float>& b, float thresh = 0.05f)
{
    int peaks = 0;
    bool inPeak = false;
    for (int i = 0; i < b.getNumSamples(); ++i) {
        const bool over = std::abs(b.getSample(0, i)) > thresh;
        if (over && !inPeak) ++peaks;
        inPeak = over;
    }
    return peaks;
}

} // namespace

TEST_CASE("Delta nulls against a latency-bearing hosted plugin", "[plugin][cq02]")
{
    ProcessorFixture f;
    constexpr int kLatency = 333; // deliberately not a block multiple
    f.addFakeLatencyPlugin(kLatency);
    f.proc.setAbMode(2); // Delta
    f.prime(4);          // fill both wet and dry delay lines

    auto out = f.impulseResponse(4);
    float maxAbs = 0.0f;
    for (int c = 0; c < 2; ++c)
        maxAbs = juce::jmax(maxAbs, out.getMagnitude(c, 0, out.getNumSamples()));

    // A unity chain through a pure delay must null once the dry path is
    // delayed by lookahead + hosted latency. Without the CQ-02 hosted-latency
    // term this leaves two full-scale impulses ~7 ms apart.
    REQUIRE(maxAbs < 1e-4f);
}

TEST_CASE("Original (A) keeps wet-path timing with hosted latency", "[plugin][cq02]")
{
    ProcessorFixture f;
    constexpr int kLatency = 250;
    f.addFakeLatencyPlugin(kLatency);

    // Reference: wet path timing (A/B off).
    f.prime(4);
    const int wetPeak = firstPeakIndex(f.impulseResponse(4));
    REQUIRE(wetPeak >= 0);

    // Original: same impulse must land at the same position.
    f.proc.setAbMode(1);
    f.prime(4);
    const int dryPeak = firstPeakIndex(f.impulseResponse(4));
    REQUIRE(dryPeak == wetPeak);
}

TEST_CASE("Original (A) stays aligned behind more than a second of hosted latency (FIX-14)",
          "[plugin][cq02][fix14]")
{
    ProcessorFixture f;
    constexpr int kLatency = 72000; // 1.5 s at 48 kHz: past the A/B line's first second
    f.addFakeLatencyPlugin(kLatency);
    const int blocks = kLatency / kBlock + 6;

    f.prime(2);
    const int wetPeak = firstPeakIndex(f.impulseResponse(blocks));
    REQUIRE(wetPeak >= kLatency);

    // The A/B dry line grew off the audio thread when the chain reported its
    // latency; with it capped at one second, Original arrived ~0.5 s early.
    f.proc.setAbMode(1);
    f.prime(blocks);
    const int dryPeak = firstPeakIndex(f.impulseResponse(blocks));
    CHECK(dryPeak == wetPeak);
}

TEST_CASE("Partial wet does not comb-filter a latency-bearing slot", "[plugin][cq02]")
{
    ProcessorFixture f;
    constexpr int kLatency = 400;
    f.addFakeLatencyPlugin(kLatency);
    f.proc.setHostedPluginWetMix(0, 0.5f);
    f.prime(4);

    auto out = f.impulseResponse(4);
    // With the per-slot dry compensation, 0.5*wet(delayed L) + 0.5*dry(delayed
    // L) is ONE impulse at L. The pre-fix behaviour was two impulses (0 and L).
    REQUIRE(countPeaks(out, 0.1f) == 1);
    const int peak = firstPeakIndex(out, 0.1f);
    REQUIRE(peak >= kLatency); // delayed by at least the slot latency
    REQUIRE(out.getSample(0, peak) == Catch::Approx(1.0f).margin(0.05));
}

TEST_CASE("Reported latency matches the routed path", "[plugin][cq02][fix14]")
{
    ProcessorFixture f;
    const int base = f.proc.getLatencySamples(); // lookahead only
    REQUIRE(base > 0);

    constexpr int kLatency = 512;
    f.addFakeLatencyPlugin(kLatency);
    REQUIRE(f.proc.getLatencySamples() == base + kLatency);

    // AUDIT-061: the dry side is always delayed to match, so the wet amount
    // (even 0) never changes the reported latency - no jump mid-drag.
    f.proc.setHostedPluginWetMix(0, 0.0f);
    REQUIRE(f.proc.getLatencySamples() == base + kLatency);
    f.proc.setHostedPluginWetMix(0, 0.005f);
    REQUIRE(f.proc.getLatencySamples() == base + kLatency);

    f.proc.setHostedPluginWetMix(0, 1.0f);
    REQUIRE(f.proc.getLatencySamples() == base + kLatency);

    // FIX-14: nor does bypassing the slot. It then plays the dry signal
    // delayed by the same amount, so the host never re-plans its compensation.
    f.proc.setHostedPluginBypass(0, true);
    REQUIRE(f.proc.getLatencySamples() == base + kLatency);
}

TEST_CASE("Bypassing a hosted slot leaves the signal where the wet path put it", "[plugin][fix14]")
{
    ProcessorFixture f;
    constexpr int kLatency = 250;
    f.addFakeLatencyPlugin(kLatency);
    f.prime(4);
    const int wetPeak = firstPeakIndex(f.impulseResponse(4));
    REQUIRE(wetPeak >= kLatency);

    f.proc.setHostedPluginBypass(0, true);
    f.prime(4); // past the bypass fade
    const auto out = f.impulseResponse(4);
    REQUIRE(countPeaks(out, 0.1f) == 1);
    REQUIRE(firstPeakIndex(out, 0.1f) == wetPeak);
}

TEST_CASE("A hosted plugin's latency change after insert is re-reported to the host", "[plugin][audit026]")
{
    ProcessorFixture f;
    const int base = f.proc.getLatencySamples();
    auto fake = std::make_unique<FakeLatencyInstance>(128);
    auto* raw = fake.get();
    fake->prepareToPlay(kSr, kBlock);
    f.proc.addPluginInstanceToChain(std::move(fake), "FakeLatency");
    REQUIRE(f.proc.getLatencySamples() == base + 128);

    // The plugin switches modes and re-reports (from whatever thread); the
    // processor picks it up on the message thread via its AsyncUpdater.
    raw->changeReportedLatency(700);
    for (int i = 0; i < 20 && f.proc.getLatencySamples() != base + 700; ++i)
        juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
    CHECK(f.proc.getLatencySamples() == base + 700);
}

TEST_CASE("The processor reports the hosted chain's tail to the host", "[plugin][audit062]")
{
    ProcessorFixture f;
    CHECK(f.proc.getTailLengthSeconds() == Catch::Approx(0.0));
    auto reverb = std::make_unique<FakeLatencyInstance>(0, 3.5);
    f.proc.addPluginInstanceToChain(std::move(reverb), "Reverb");
    CHECK(f.proc.getTailLengthSeconds() == Catch::Approx(3.5));
    f.proc.setHostedPluginBypass(0, true);
    CHECK(f.proc.getTailLengthSeconds() == Catch::Approx(0.0));
}
