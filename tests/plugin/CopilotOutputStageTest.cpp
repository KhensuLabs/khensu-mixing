#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include "../../plugin-copilot/Source/CopilotOutputStage.h"
#include "../../plugin-copilot/Source/CopilotRealtimeTelemetry.h"

#include <cmath>
#include <limits>

using khensu::CopilotOutputStage;

namespace {
constexpr double kRate = 48000.0;
constexpr int kBlock = 480;

void fillSine(juce::AudioBuffer<float>& b, float amp, int& phase)
{
    for (int i = 0; i < b.getNumSamples(); ++i, ++phase)
        for (int c = 0; c < b.getNumChannels(); ++c)
            b.setSample(c, i, amp * static_cast<float>(std::sin(2.0 * juce::MathConstants<double>::pi * 997.0 * phase / kRate)));
}

double rms(const juce::AudioBuffer<float>& b)
{
    double s = 0.0;
    for (int c = 0; c < b.getNumChannels(); ++c)
        for (int i = 0; i < b.getNumSamples(); ++i) s += double(b.getSample(c, i)) * b.getSample(c, i);
    return std::sqrt(s / (b.getNumChannels() * b.getNumSamples()));
}
} // namespace

TEST_CASE("Bypass delays the dry signal by the full reported latency", "[plugin][audit]")
{
    CopilotOutputStage stage;
    stage.prepareToPlay(kRate, kBlock, 2);
    stage.snapBypass(true); // already bypassed: no switch crossfade in play
    const int hostedLatency = 100;
    const int expectedDelay = stage.latencySamples() + hostedLatency;

    juce::AudioBuffer<float> buf(2, kBlock);
    buf.clear();
    buf.setSample(0, 0, 1.0f);
    buf.setSample(1, 0, 1.0f);
    stage.captureDry(buf, 0);
    stage.process(buf, /*globalBypass*/ true, true, -1.0f, 0, 0, hostedLatency);

    int impulseAt = -1;
    for (int i = 0; i < kBlock; ++i)
        if (buf.getSample(0, i) > 0.5f) impulseAt = i;
    CHECK(impulseAt == expectedDelay);
}

TEST_CASE("A/B Original side is loudness matched to the processed side", "[plugin][audit]")
{
    CopilotOutputStage stage;
    stage.prepareToPlay(kRate, kBlock, 2);
    int phase = 0;
    juce::AudioBuffer<float> buf(2, kBlock);
    double lastRms = 0.0;
    // "Processing" = +6 dB gain applied between captureDry and process.
    for (int block = 0; block < 2000; ++block) {
        fillSine(buf, 0.1f, phase);
        stage.captureDry(buf, 1);
        buf.applyGain(2.0f);
        stage.process(buf, false, false, 0.0f, /*Original*/ 1, 0);
        lastRms = rms(buf);
    }
    // Original plays at the processed level (0.2 amplitude sine), not 0.1.
    CHECK(lastRms == Catch::Approx(0.2 / std::sqrt(2.0)).epsilon(0.02));
    CHECK(stage.matchGainDb() == Catch::Approx(6.02f).margin(0.1f));
}

TEST_CASE("Processed mode output is not altered by level matching", "[plugin][audit]")
{
    CopilotOutputStage stage;
    stage.prepareToPlay(kRate, kBlock, 2);
    int phase = 0;
    juce::AudioBuffer<float> buf(2, kBlock);
    for (int block = 0; block < 500; ++block) {
        fillSine(buf, 0.1f, phase);
        stage.captureDry(buf, 0);
        buf.applyGain(2.0f);
        stage.process(buf, false, false, 0.0f, 0, 0);
    }
    // Only the (constant) lookahead delay applies: level is the processed 0.2.
    CHECK(rms(buf) == Catch::Approx(0.2 / std::sqrt(2.0)).epsilon(0.01));
}

TEST_CASE("Mono telemetry loudness is not +3 dB high", "[plugin][audit]")
{
    // A full-scale-ish 997 Hz sine in ONE channel and the same sine in a
    // stereo pair's left channel only must read the same loudness.
    khensu::CopilotRealtimeTelemetry mono, stereo;
    mono.prepareToPlay(kRate, 1);
    stereo.prepareToPlay(kRate, 2);
    int phaseA = 0, phaseB = 0;
    juce::AudioBuffer<float> m(1, kBlock), s(2, kBlock);
    for (int block = 0; block < 600; ++block) {
        fillSine(m, 0.1f, phaseA);
        fillSine(s, 0.1f, phaseB);
        s.clear(1, 0, kBlock);
        mono.process(m);
        stereo.process(s);
    }
    CHECK(mono.snapshot().short_term_lufs == Catch::Approx(stereo.snapshot().short_term_lufs).margin(0.05f));
}

namespace {
/// Drives the stage with a constant 0.5 input whose "processing" inverts the
/// polarity, so Processed / Original / bypassed differ by the full 1.0 swing
/// and a hard cut would show up as a 1.0 jump between two samples. Returns
/// the largest sample-to-sample step over `blocks` blocks (block seams
/// included) and leaves the last output in `buf`.
float runInverted(CopilotOutputStage& stage, juce::AudioBuffer<float>& buf, int blocks, int abMode, bool bypass,
                  float& previous)
{
    float maxStep = 0.0f;
    for (int block = 0; block < blocks; ++block) {
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < kBlock; ++i) buf.setSample(c, i, 0.5f);
        stage.captureDry(buf, abMode);
        if (stage.needsProcessedPath(bypass)) buf.applyGain(-1.0f); // the "chain"
        stage.process(buf, bypass, false, 0.0f, abMode, 0);
        for (int i = 0; i < kBlock; ++i) {
            maxStep = juce::jmax(maxStep, std::abs(buf.getSample(0, i) - previous));
            previous = buf.getSample(0, i);
        }
    }
    return maxStep;
}
} // namespace

TEST_CASE("A/B mode switches crossfade instead of hard-cutting", "[plugin][audit086]")
{
    CopilotOutputStage stage;
    stage.prepareToPlay(kRate, kBlock, 2);
    const int fade = static_cast<int>(std::round(kRate * CopilotOutputStage::kSwitchFadeSeconds));
    juce::AudioBuffer<float> buf(2, kBlock);
    float previous = 0.0f;

    runInverted(stage, buf, 20, /*Processed*/ 0, false, previous);
    CHECK(buf.getSample(0, kBlock - 1) == Catch::Approx(-0.5f));

    const float maxStep = runInverted(stage, buf, 4, /*Original*/ 1, false, previous);
    // The full -0.5 -> +0.5 swing is spread over the fade.
    CHECK(maxStep <= 1.0f / static_cast<float>(fade) + 1.0e-4f);
    CHECK(buf.getSample(0, kBlock - 1) == Catch::Approx(0.5f).margin(1.0e-3));
}

TEST_CASE("Global bypass fades to the aligned dry and reports when the chain may stop", "[plugin][audit086]")
{
    CopilotOutputStage stage;
    stage.prepareToPlay(kRate, kBlock, 2);
    const int fade = static_cast<int>(std::round(kRate * CopilotOutputStage::kSwitchFadeSeconds));
    juce::AudioBuffer<float> buf(2, kBlock);
    float previous = 0.0f;

    runInverted(stage, buf, 10, 0, false, previous);
    CHECK(buf.getSample(0, kBlock - 1) == Catch::Approx(-0.5f));

    CHECK(stage.needsProcessedPath(true)); // the fade still needs the processed side
    float maxStep = runInverted(stage, buf, 1, 0, true, previous);
    CHECK(stage.needsProcessedPath(true)); // 480 samples < the 720-sample fade
    maxStep = juce::jmax(maxStep, runInverted(stage, buf, 3, 0, true, previous));
    CHECK(maxStep <= 1.0f / static_cast<float>(fade) + 1.0e-4f);
    CHECK(buf.getSample(0, kBlock - 1) == Catch::Approx(0.5f));
    CHECK_FALSE(stage.needsProcessedPath(true)); // settled: chain may be skipped

    // ...and back in, again without a hard cut.
    maxStep = runInverted(stage, buf, 4, 0, false, previous);
    CHECK(maxStep <= 1.0f / static_cast<float>(fade) + 1.0e-4f);
    CHECK(buf.getSample(0, kBlock - 1) == Catch::Approx(-0.5f));
}

TEST_CASE("Non-finite level-match history is dropped instead of disabling A/B", "[plugin][audit025]")
{
    CopilotOutputStage stage;
    stage.prepareToPlay(kRate, kBlock, 2);
    int phase = 0;
    juce::AudioBuffer<float> buf(2, kBlock);
    fillSine(buf, 0.1f, phase);
    stage.captureDry(buf, 1);
    buf.setSample(0, 10, std::numeric_limits<float>::infinity());
    stage.process(buf, false, false, 0.0f, 1, 0);

    // Fresh, finite audio: matching recovers (no NaN gain stuck forever).
    for (int block = 0; block < 2000; ++block) {
        fillSine(buf, 0.1f, phase);
        stage.captureDry(buf, 1);
        buf.applyGain(2.0f);
        stage.process(buf, false, false, 0.0f, 1, 0);
    }
    CHECK(std::isfinite(stage.matchGainDb()));
    CHECK(stage.matchGainDb() == Catch::Approx(6.02f).margin(0.2f));
    CHECK(std::isfinite(rms(buf)));
}
