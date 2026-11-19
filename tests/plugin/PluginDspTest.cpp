// tests/plugin/PluginDspTest.cpp
// Regression tests for the 2026-11-06 audit's plugin DSP findings:
//   PLG-01  coefficient updates are never lost (two publishes, no block between)
//   PLG-08  offline automation is deterministic (no wall-clock worker)
//   PLG-02 / CORE-02  the oversampled true-peak limiter holds its ceiling
//   PLG-03  leaving bypass does not replay stale hosted-plugin audio
//   PLG-13  momentary / short-term loudness use BS.1770 rectangular windows
//   PLG-14  stage on/off switches crossfade instead of clicking
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "../../plugin-copilot/Source/CopilotCorrectiveDsp.h"
#include "../../plugin-copilot/Source/CopilotOutputStage.h"
#include "../../plugin-copilot/Source/CopilotRealtimeTelemetry.h"
#include "../../plugin-copilot/Source/PluginProcessor.h"
#include "FakeHostedInstances.h"

#include <cmath>
#include <vector>

using khensu::CopilotCorrectiveDsp;
using khensu::CopilotOutputStage;
using khensu::CopilotRealtimeTelemetry;
using khensu::KhensuCopilotProcessor;
using P = khensu::KhensuCopilotProcessor::ParamIDs;

namespace {
constexpr double kPi = 3.14159265358979323846;

/// Owns the atomics a CopilotCorrectiveDsp is bound to.
struct DspParams {
    std::atomic<float> chainMix{1.0f}, hpfOn{0}, hpfFreq{80}, lpfOn{0}, lpfFreq{18000}, eqOn{0}, eqFreq{1000},
        eqGainDb{0}, eqQ{1}, eq2On{0}, eq2Freq{1000}, eq2GainDb{0}, eq2Q{1}, eq3On{0}, eq3Freq{1000}, eq3GainDb{0},
        eq3Q{1}, compOn{0}, compThreshDb{-18}, compRatio{4}, compAttackMs{5}, compReleaseMs{100}, compMultiband{0},
        compLowThreshDb{-18}, compMidThreshDb{-18}, compHighThreshDb{-18}, crossoverLowFreq{200},
        crossoverHighFreq{2500}, outGainDb{0};

    CopilotCorrectiveDsp::Parameters bind()
    {
        CopilotCorrectiveDsp::Parameters p;
        p.chainMix = &chainMix;
        p.hpfOn = &hpfOn;
        p.hpfFreq = &hpfFreq;
        p.lpfOn = &lpfOn;
        p.lpfFreq = &lpfFreq;
        p.eqOn = &eqOn;
        p.eqFreq = &eqFreq;
        p.eqGainDb = &eqGainDb;
        p.eqQ = &eqQ;
        p.eq2On = &eq2On;
        p.eq2Freq = &eq2Freq;
        p.eq2GainDb = &eq2GainDb;
        p.eq2Q = &eq2Q;
        p.eq3On = &eq3On;
        p.eq3Freq = &eq3Freq;
        p.eq3GainDb = &eq3GainDb;
        p.eq3Q = &eq3Q;
        p.compOn = &compOn;
        p.compThreshDb = &compThreshDb;
        p.compRatio = &compRatio;
        p.compAttackMs = &compAttackMs;
        p.compReleaseMs = &compReleaseMs;
        p.compMultiband = &compMultiband;
        p.compLowThreshDb = &compLowThreshDb;
        p.compMidThreshDb = &compMidThreshDb;
        p.compHighThreshDb = &compHighThreshDb;
        p.crossoverLowFreq = &crossoverLowFreq;
        p.crossoverHighFreq = &crossoverHighFreq;
        p.outGainDb = &outGainDb;
        return p;
    }
};

void fillSine(juce::AudioBuffer<float>& b, double hz, double sr, float amp, long long& phase)
{
    for (int i = 0; i < b.getNumSamples(); ++i) {
        const float v = amp * static_cast<float>(std::sin(2.0 * kPi * hz * static_cast<double>(phase + i) / sr));
        for (int c = 0; c < b.getNumChannels(); ++c) b.setSample(c, i, v);
    }
    phase += b.getNumSamples();
}

float dbOf(float gain) { return 20.0f * std::log10(juce::jmax(1.0e-9f, gain)); }

/// 4x true-peak estimate of a mono signal with the same FIR oversampler the
/// output stage's detector uses.
float truePeak(const std::vector<float>& signal, double sr)
{
    juce::dsp::Oversampling<float> os(1, 2, juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple, true);
    constexpr int kBlock = 256;
    os.initProcessing(kBlock);
    juce::ignoreUnused(sr);
    juce::AudioBuffer<float> block(1, kBlock);
    float peak = 0.0f;
    for (size_t start = 0; start < signal.size(); start += kBlock) {
        block.clear();
        for (int i = 0; i < kBlock && start + (size_t)i < signal.size(); ++i)
            block.setSample(0, i, signal[start + (size_t)i]);
        juce::dsp::AudioBlock<const float> in(block);
        auto up = os.processSamplesUp(in);
        for (size_t i = 0; i < up.getNumSamples(); ++i) peak = juce::jmax(peak, std::abs(up.getSample(0, (int)i)));
    }
    return peak;
}
} // namespace

TEST_CASE("Two coefficient publishes with no audio block between land the final value (PLG-01)",
          "[plugin][plg01]")
{
    constexpr double sr = 48000.0;
    constexpr int block = 512;
    DspParams params;
    params.eqOn = 1.0f;
    CopilotCorrectiveDsp dsp;
    dsp.bindParameters(params.bind());
    dsp.prepareToPlay(sr, block, 2);

    // Two parameter changes with no processBlock in between (a host that
    // stops calling process while the transport is stopped). The old worker
    // alternated snapshot slots and could leave the filter on the first value.
    params.eqGainDb = 3.0f;
    params.eqGainDb = 6.0f;

    juce::AudioBuffer<float> buf(2, block);
    long long phase = 0;
    double inSum = 0.0, outSum = 0.0;
    for (int b = 0; b < 40; ++b) { // ~430 ms: far past the 20 ms ramp
        fillSine(buf, 1000.0, sr, 0.1f, phase);
        double in = 0.0;
        for (int i = 0; i < block; ++i) in += (double)buf.getSample(0, i) * buf.getSample(0, i);
        dsp.process(buf, false, block);
        if (b >= 30) {
            inSum += in;
            for (int i = 0; i < block; ++i) outSum += (double)buf.getSample(0, i) * buf.getSample(0, i);
        }
    }
    CHECK(dsp.currentEqGainDbForTest() == Catch::Approx(6.0f).margin(1e-4f));
    // A peak filter at its centre frequency applies exactly its gain.
    CHECK(10.0 * std::log10(outSum / inSum) == Catch::Approx(6.0).margin(0.15));
}

TEST_CASE("Parameter smoothing advances in audio time, not wall-clock time (PLG-08)", "[plugin][plg08]")
{
    constexpr double sr = 48000.0;
    constexpr int block = 256;
    DspParams params;
    params.eqOn = 1.0f;
    CopilotCorrectiveDsp dsp;
    dsp.bindParameters(params.bind());
    dsp.prepareToPlay(sr, block, 2);
    params.eqGainDb = 12.0f;

    // Long wall-clock pause, no audio: nothing may move (the worker used to
    // finish the ramp in the background).
    juce::Thread::sleep(50);
    CHECK(dsp.currentEqGainDbForTest() == Catch::Approx(0.0f).margin(1e-4f));

    // Half the 20 ms ramp of audio moves it about half way.
    juce::AudioBuffer<float> buf(2, 240);
    buf.clear();
    dsp.process(buf, false, 240);
    dsp.process(buf, false, 240);
    CHECK(dsp.currentEqGainDbForTest() == Catch::Approx(6.0f).margin(0.5f));
}

TEST_CASE("Offline renders with parameter automation are bit-identical (PLG-08)", "[plugin][plg08]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    constexpr double sr = 44100.0;
    constexpr int block = 512;
    auto render = [&]() {
        KhensuCopilotProcessor proc;
        proc.setPlayConfigDetails(2, 2, sr, block);
        proc.setNonRealtime(true);
        proc.setParamThreadSafe(P::eqOn, 1.0f);
        proc.setParamThreadSafe(P::hpfOn, 1.0f);
        proc.prepareToPlay(sr, block);
        juce::AudioBuffer<float> buf(2, block);
        juce::MidiBuffer midi;
        long long phase = 0;
        std::vector<float> out;
        for (int b = 0; b < 60; ++b) {
            // Automation lands between blocks, as a host renders it.
            proc.setParamThreadSafe(P::eqGainDb, static_cast<float>((b % 7) * 2 - 6));
            proc.setParamThreadSafe(P::hpfFreq, 60.0f + static_cast<float>((b % 5) * 80));
            fillSine(buf, 440.0, sr, 0.3f, phase);
            proc.processBlock(buf, midi);
            out.insert(out.end(), buf.getReadPointer(0), buf.getReadPointer(0) + block);
            if (b % 9 == 0) juce::Thread::sleep(3); // wall-clock jitter must not matter
        }
        return out;
    };
    const auto a = render();
    const auto b = render();
    REQUIRE(a.size() == b.size());
    size_t differing = 0;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i] != b[i]) ++differing;
    CHECK(differing == 0);
}

TEST_CASE("The output stage aligns the oversampled detector with the audio (CORE-02)", "[plugin][plg02][core02]")
{
    CopilotOutputStage stage;
    stage.prepareToPlay(48000.0, 512, 2);
    // JUCE's 2-stage FIR upsampler lags ~36 base-rate samples.
    CHECK(stage.detectorDelaySamples() > 20.0f);
    CHECK(stage.alignSamples() >= static_cast<int>(std::ceil(stage.detectorDelaySamples())));
    CHECK(stage.latencySamples() == stage.lookaheadSamples() + stage.alignSamples());
}

TEST_CASE("Limiter ceiling holds through the oversampled output stage at 44.1/48/96 kHz (PLG-02)",
          "[plugin][plg02][core02]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    const float ceilingDb = -1.0f;
    const float ceiling = juce::Decibels::decibelsToGain(ceilingDb);
    for (double sr : {44100.0, 48000.0, 96000.0}) {
        INFO("sample rate " << sr);
        KhensuCopilotProcessor proc;
        constexpr int block = 512;
        proc.setPlayConfigDetails(2, 2, sr, block);
        proc.setParamThreadSafe(P::limOn, 1.0f);
        proc.setParamThreadSafe(P::limThreshDb, ceilingDb);
        proc.prepareToPlay(sr, block);
        CHECK(proc.getLatencySamples() > static_cast<int>(std::round(sr * 0.0015))); // lookahead + alignment

        // Quiet bed, then hard +6 dBFS steps, then fs/4 bursts whose samples
        // sit below the ceiling but whose true peak is +1.6 dBFS over it.
        const int total = static_cast<int>(sr * 1.2);
        std::vector<float> input(static_cast<size_t>(total));
        for (int i = 0; i < total; ++i) {
            const double t = i / sr;
            float v = 0.05f * static_cast<float>(std::sin(2.0 * kPi * 220.0 * t));
            const int cycle = i % static_cast<int>(sr * 0.2);
            if (cycle > sr * 0.05 && cycle < sr * 0.08) v = (i / 7) % 2 == 0 ? 2.0f : -2.0f; // steps
            if (cycle > sr * 0.12 && cycle < sr * 0.15)
                v = 1.2f * static_cast<float>(std::sin(kPi / 2.0 * i + kPi / 4.0)); // intersample overs
            input[static_cast<size_t>(i)] = v;
        }
        juce::AudioBuffer<float> buf(2, block);
        juce::MidiBuffer midi;
        std::vector<float> output;
        for (int start = 0; start < total; start += block) {
            const int n = juce::jmin(block, total - start);
            juce::AudioBuffer<float> view(buf.getArrayOfWritePointers(), 2, 0, n);
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < n; ++i) view.setSample(c, i, input[static_cast<size_t>(start + i)]);
            proc.processBlock(view, midi);
            output.insert(output.end(), view.getReadPointer(0), view.getReadPointer(0) + n);
        }
        float samplePeak = 0.0f;
        for (float v : output) samplePeak = juce::jmax(samplePeak, std::abs(v));
        const float tp = truePeak(output, sr);
        UNSCOPED_INFO("sample peak " << dbOf(samplePeak) << " dBFS, true peak " << dbOf(tp) << " dBFS");
        CHECK(dbOf(samplePeak) <= ceilingDb + 0.1f);
        CHECK(dbOf(tp) <= ceilingDb + 0.1f);
        CHECK(samplePeak > ceiling * 0.5f); // the limiter limited, it did not mute
    }
}

TEST_CASE("Leaving bypass does not replay stale audio from a latency-bearing hosted plugin (PLG-03)",
          "[plugin][plg03]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    constexpr double sr = 48000.0;
    constexpr int block = 256;
    KhensuCopilotProcessor proc;
    proc.setPlayConfigDetails(2, 2, sr, block);
    proc.prepareToPlay(sr, block);
    auto fake = std::make_unique<khensu::test::FakeLatencyInstance>(2000);
    fake->prepareToPlay(sr, block);
    REQUIRE(proc.addPluginInstanceToChain(std::move(fake), "Lookahead", "C:/plugins/lookahead.vst3"));

    juce::AudioBuffer<float> buf(2, block);
    juce::MidiBuffer midi;
    auto run = [&](float level, int blocks, float& maxOut) {
        for (int b = 0; b < blocks; ++b) {
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < block; ++i) buf.setSample(c, i, level);
            proc.processBlock(buf, midi);
            for (int i = 0; i < block; ++i) maxOut = juce::jmax(maxOut, std::abs(buf.getSample(0, i)));
        }
    };
    float ignored = 0.0f;
    run(0.9f, 40, ignored); // the hosted plugin's delay line fills with 0.9
    proc.setParamThreadSafe(P::bypass, 1.0f);
    run(0.2f, 60, ignored); // bypass settles; the hosted chain stops being fed
    proc.setParamThreadSafe(P::bypass, 0.0f);
    float maxAfter = 0.0f;
    run(0.2f, 60, maxAfter);
    // Only 0.2 went in since long before un-bypassing; a replay of the
    // pre-bypass 0.9 held inside the hosted plugin would show up here.
    CHECK(maxAfter < 0.25f);
}

TEST_CASE("Momentary and short-term loudness use rectangular BS.1770 windows (PLG-13)", "[plugin][plg13]")
{
    constexpr double sr = 48000.0;
    constexpr int block = 480;
    CopilotRealtimeTelemetry telemetry;
    telemetry.prepareToPlay(sr, 2);
    juce::AudioBuffer<float> buf(2, block);
    long long phase = 0;
    // EBU Tech 3341: a stereo 1 kHz sine with -23 dBFS peaks reads -23 LUFS.
    const float peakAmp = juce::Decibels::decibelsToGain(-23.0f);
    for (int b = 0; b < 400; ++b) { // 4 s
        fillSine(buf, 997.0, sr, peakAmp, phase);
        telemetry.process(buf);
    }
    CHECK(telemetry.snapshot().momentary_lufs == Catch::Approx(-23.0f).margin(0.3f));
    CHECK(telemetry.snapshot().short_term_lufs == Catch::Approx(-23.0f).margin(0.3f));

    // 200 ms of tone then 200 ms of silence: a 400 ms rectangular window
    // holds exactly half the energy (-3.01 LU)...
    CopilotRealtimeTelemetry burst;
    burst.prepareToPlay(sr, 2);
    phase = 0;
    for (int b = 0; b < 20; ++b) {
        fillSine(buf, 997.0, sr, peakAmp, phase);
        burst.process(buf);
    }
    buf.clear();
    for (int b = 0; b < 20; ++b) burst.process(buf);
    CHECK(burst.snapshot().momentary_lufs == Catch::Approx(-26.0f).margin(0.3f));
    // ...and 500 ms of silence after it the window is empty (a one-pole
    // average still read about -27 LUFS here).
    for (int b = 0; b < 30; ++b) burst.process(buf);
    CHECK(burst.snapshot().momentary_lufs <= -90.0f);
}

TEST_CASE("Switching a stage on or off crossfades instead of clicking (PLG-14)", "[plugin][plg14]")
{
    constexpr double sr = 48000.0;
    constexpr int block = 256;
    DspParams params;
    params.eqGainDb = 12.0f;
    params.eqQ = 0.7f;
    params.eqFreq = 1000.0f;
    CopilotCorrectiveDsp dsp;
    dsp.bindParameters(params.bind());
    dsp.prepareToPlay(sr, block, 2);

    juce::AudioBuffer<float> buf(2, block);
    long long phase = 0;
    float prev = 0.0f, maxStep = 0.0f;
    for (int b = 0; b < 80; ++b) {
        if (b == 20) params.eqOn = 1.0f; // +12 dB bell switched in
        if (b == 50) params.eqOn = 0.0f; // and out again
        fillSine(buf, 1000.0, sr, 0.2f, phase);
        dsp.process(buf, false, block);
        for (int i = 0; i < block; ++i) {
            maxStep = juce::jmax(maxStep, std::abs(buf.getSample(0, i) - prev));
            prev = buf.getSample(0, i);
        }
    }
    // The boosted sine (~0.8 peak) moves at most ~0.105 per sample at 1 kHz;
    // a hard cut jumps by up to the full difference between the two paths.
    INFO("max sample step " << maxStep);
    CHECK(maxStep < 0.15f);
}
