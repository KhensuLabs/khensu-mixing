// tests/plugin/CopilotReferenceMonitorTest.cpp
// ENH-10 (1) and (2): the reference track's loudness measure and level
// matching, and the monitoring solo modes - on their own and inside the
// processor, including the real-time (no allocation) contract and offline
// renders hearing neither.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "../../plugin-copilot/Source/PluginProcessor.h"
#include "../../plugin-copilot/Source/monitor/MonitorSolo.h"
#include "../../plugin-copilot/Source/reference/LoudnessStats.h"
#include "../../plugin-copilot/Source/reference/ReferenceTrack.h"
#include "AllocationGuard.h"

#include <cmath>

using khensu::KhensuCopilotProcessor;
using khensu::copilot::MonitorSolo;
using khensu::copilot::reference::LoudnessStats;
using khensu::copilot::reference::ReferencePlayer;

namespace {
constexpr double kSr = 48000.0;
constexpr int kBlock = 512;
constexpr double kPi = 3.14159265358979323846;

juce::AudioBuffer<float> sine(double hz, float amplitude, int samples, bool invertRight = false, int channels = 2)
{
    juce::AudioBuffer<float> b(channels, samples);
    for (int i = 0; i < samples; ++i) {
        const auto v = static_cast<float>(amplitude * std::sin(2.0 * kPi * hz * i / kSr));
        b.setSample(0, i, v);
        if (channels > 1) b.setSample(1, i, invertRight ? -v : v);
    }
    return b;
}

float rms(const juce::AudioBuffer<float>& b, int channel = 0)
{
    double sum = 0.0;
    const float* d = b.getReadPointer(channel);
    for (int i = 0; i < b.getNumSamples(); ++i) sum += static_cast<double>(d[i]) * d[i];
    return static_cast<float>(std::sqrt(sum / juce::jmax(1, b.getNumSamples())));
}

float maxAbs(const juce::AudioBuffer<float>& b)
{
    float m = 0.0f;
    for (int c = 0; c < b.getNumChannels(); ++c) m = juce::jmax(m, b.getMagnitude(c, 0, b.getNumSamples()));
    return m;
}

int signChanges(const juce::AudioBuffer<float>& b)
{
    int n = 0;
    const float* d = b.getReadPointer(0);
    for (int i = 1; i < b.getNumSamples(); ++i)
        if ((d[i - 1] < 0.0f) != (d[i] < 0.0f)) ++n;
    return n;
}

/// Feeds `seconds` of a host sine through the processor in blocks, and
/// returns the last block it put out.
juce::AudioBuffer<float> run(KhensuCopilotProcessor& proc, double hz, float amplitude, double seconds,
                             juce::int64& phase, LoudnessStats* outStats = nullptr, bool guarded = false)
{
    juce::AudioBuffer<float> buf(2, kBlock);
    juce::MidiBuffer midi;
    const int blocks = static_cast<int>(seconds * kSr / kBlock);
    for (int b = 0; b < blocks; ++b) {
        for (int i = 0; i < kBlock; ++i) {
            const auto v = static_cast<float>(amplitude * std::sin(2.0 * kPi * hz * static_cast<double>(phase + i) / kSr));
            buf.setSample(0, i, v);
            buf.setSample(1, i, v);
        }
        phase += kBlock;
        if (guarded) {
            khensu::test::ScopedAllocationGuard guard;
            proc.processBlock(buf, midi);
        } else {
            proc.processBlock(buf, midi);
        }
        if (outStats != nullptr) outStats->process(buf.getArrayOfReadPointers(), 2, kBlock);
    }
    return buf;
}

struct Fixture {
    juce::ScopedJuceInitialiser_GUI juceInit;
    KhensuCopilotProcessor proc;
    Fixture()
    {
        proc.setPlayConfigDetails(2, 2, kSr, kBlock);
        proc.prepareToPlay(kSr, kBlock);
    }
};
} // namespace

TEST_CASE("The reference loudness measure reads a stereo sine as BS.1770 does (ENH-10)", "[plugin][enh10][reference]")
{
    LoudnessStats stats;
    stats.prepare(kSr);
    // A 1 kHz sine at -20 dBFS in both channels is -20 LUFS (EBU Tech 3341).
    const auto audio = sine(1000.0, std::pow(10.0f, -20.0f / 20.0f), static_cast<int>(kSr * 10));
    stats.process(audio.getArrayOfReadPointers(), 2, audio.getNumSamples());
    const auto r = stats.result();
    CHECK(r.valid);
    CHECK(r.integratedLufs == Catch::Approx(-20.0f).margin(0.25f));
    CHECK(r.shortTermLufs == Catch::Approx(-20.0f).margin(0.25f));
    CHECK(r.loudnessRangeLu < 0.5f);               // a steady tone has no range
    CHECK(r.crestDb == Catch::Approx(3.01f).margin(0.15f)); // a sine's peak-to-RMS
    CHECK(r.peakDb == Catch::Approx(-20.0f).margin(0.05f));

    // A quiet half and a loud half: a range of about 10 LU.
    LoudnessStats steps;
    steps.prepare(kSr);
    const auto quiet = sine(1000.0, std::pow(10.0f, -30.0f / 20.0f), static_cast<int>(kSr * 20));
    const auto loud = sine(1000.0, std::pow(10.0f, -20.0f / 20.0f), static_cast<int>(kSr * 20));
    steps.process(quiet.getArrayOfReadPointers(), 2, quiet.getNumSamples());
    steps.process(loud.getArrayOfReadPointers(), 2, loud.getNumSamples());
    CHECK(steps.result().loudnessRangeLu == Catch::Approx(10.0f).margin(1.0f));
}

TEST_CASE("The reference plays loudness matched to the track (ENH-10)", "[plugin][enh10][reference]")
{
    Fixture f;
    // Reference: a 3 kHz tone 14 dB louder than the track.
    auto refAudio = sine(3000.0, 0.5f, static_cast<int>(kSr * 10));
    auto ref = khensu::copilot::reference::analyseReference(std::move(refAudio), kSr, "ref.wav");
    REQUIRE(ref != nullptr);
    CHECK(ref->stats.integratedLufs > -10.0f);
    f.proc.setReferenceForTesting(ref);
    CHECK(f.proc.getReferencePath() == "ref.wav");

    juce::int64 phase = 0;
    // The track alone: a 1 kHz tone, measured on the processor's output.
    LoudnessStats trackOut;
    trackOut.prepare(kSr);
    auto last = run(f.proc, 1000.0, 0.1f, 6.0, phase, &trackOut);
    const float trackLufs = f.proc.getTrackStats().integratedLufs;
    CHECK(trackLufs == Catch::Approx(trackOut.result().integratedLufs).margin(0.3f));
    CHECK(signChanges(last) < 30); // ~21 for 1 kHz in 512 samples

    // Hear the reference: it replaces the track, turned to the track's loudness.
    f.proc.setReferenceListening(true);
    run(f.proc, 1000.0, 0.1f, 0.5, phase); // the 15 ms fade and the first match
    LoudnessStats heard;
    heard.prepare(kSr);
    last = run(f.proc, 1000.0, 0.1f, 5.0, phase, &heard);
    CHECK(signChanges(last) > 50); // ~64 for 3 kHz: the reference is what plays
    CHECK(f.proc.referencePlayer().isMatched());
    CHECK(f.proc.referencePlayer().offsetDb()
          == Catch::Approx(trackLufs - ref->stats.integratedLufs).margin(0.3f));
    CHECK(heard.result().integratedLufs == Catch::Approx(trackLufs).margin(0.5f));

    // An offline render never hears the reference.
    f.proc.setNonRealtime(true);
    last = run(f.proc, 1000.0, 0.1f, 0.2, phase);
    CHECK(signChanges(last) < 30);
    f.proc.setNonRealtime(false);

    // The path is saved with the plugin state.
    juce::MemoryBlock state;
    f.proc.getStateInformation(state);
    KhensuCopilotProcessor restored;
    restored.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    CHECK(restored.getReferencePath() == "ref.wav");
    juce::MemoryBlock again;
    restored.getStateInformation(again);
    CHECK(again == state); // byte-stable round trip
}

TEST_CASE("Short-term matching follows the reference's loudness over time (ENH-10)", "[plugin][enh10][reference]")
{
    ReferencePlayer player;
    player.prepare(kSr);
    // Reference: 6 s at -10 dBFS then 6 s at -30 dBFS.
    juce::AudioBuffer<float> audio(2, static_cast<int>(kSr * 12));
    const auto loud = sine(1000.0, std::pow(10.0f, -10.0f / 20.0f), static_cast<int>(kSr * 6));
    const auto soft = sine(1000.0, std::pow(10.0f, -30.0f / 20.0f), static_cast<int>(kSr * 6));
    for (int c = 0; c < 2; ++c) {
        audio.copyFrom(c, 0, loud, c, 0, loud.getNumSamples());
        audio.copyFrom(c, loud.getNumSamples(), soft, c, 0, soft.getNumSamples());
    }
    auto ref = khensu::copilot::reference::analyseReference(std::move(audio), kSr, "steps.wav");
    player.setReference(ref);
    player.setMatchMode(ReferencePlayer::MatchMode::ShortTerm);
    player.setListening(true);
    ReferencePlayer::HostSide host;
    host.timeValid = true;
    host.shortTermLufs = -20.0f;
    host.integratedLufs = -20.0f;
    juce::AudioBuffer<float> buf(2, kBlock);
    // At 5 s the reference is loud: it is turned down ~10 dB.
    host.timeSec = 5.0;
    for (int i = 0; i < 100; ++i) player.process(buf, host);
    CHECK(player.offsetDb() == Catch::Approx(-10.0f).margin(0.6f));
    // At 11 s it is soft: turned up ~10 dB.
    host.timeSec = 11.0;
    for (int i = 0; i < 200; ++i) player.process(buf, host);
    CHECK(player.offsetDb() == Catch::Approx(10.0f).margin(0.6f));
}

TEST_CASE("Monitoring solo: mono, side, sub and bands (ENH-10)", "[plugin][enh10][solo]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    MonitorSolo solo;
    solo.prepare(kSr, kBlock);
    const auto settle = [&solo](const juce::AudioBuffer<float>& source, int blocks) {
        juce::AudioBuffer<float> out;
        for (int b = 0; b < blocks; ++b) {
            out.makeCopyOf(source);
            solo.process(out, false);
        }
        return out;
    };

    SECTION("the mono sum of L = -R is silence")
    {
        solo.setMode(MonitorSolo::Mode::Mono);
        const auto out = settle(sine(440.0, 0.5f, kBlock, true), 4);
        CHECK(maxAbs(out) < 1.0e-6f);
    }
    SECTION("side only of L = R is silence")
    {
        solo.setMode(MonitorSolo::Mode::Side);
        CHECK(maxAbs(settle(sine(440.0, 0.5f, kBlock), 4)) < 1.0e-6f);
    }
    SECTION("sub only removes 1 kHz and keeps 60 Hz")
    {
        solo.setMode(MonitorSolo::Mode::Sub);
        // A continuous tone over many blocks, so the filters settle.
        juce::AudioBuffer<float> tone = sine(1000.0, 0.5f, static_cast<int>(kSr));
        juce::AudioBuffer<float> out;
        out.makeCopyOf(tone);
        solo.process(out, false);
        juce::AudioBuffer<float> tail(2, 4800);
        tail.copyFrom(0, 0, out, 0, out.getNumSamples() - 4800, 4800);
        CHECK(juce::Decibels::gainToDecibels(rms(tail) / rms(tone)) < -40.0f);

        MonitorSolo low;
        low.prepare(kSr, kBlock);
        low.setMode(MonitorSolo::Mode::Sub);
        juce::AudioBuffer<float> bass = sine(60.0, 0.5f, static_cast<int>(kSr));
        juce::AudioBuffer<float> bassOut;
        bassOut.makeCopyOf(bass);
        low.process(bassOut, false);
        tail.copyFrom(0, 0, bassOut, 0, bassOut.getNumSamples() - 4800, 4800);
        juce::AudioBuffer<float> bassTail(2, 4800);
        bassTail.copyFrom(0, 0, bass, 0, bass.getNumSamples() - 4800, 4800);
        CHECK(std::abs(juce::Decibels::gainToDecibels(rms(tail) / rms(bassTail))) < 1.0f);
    }
    SECTION("the high band removes 100 Hz, the low band removes 8 kHz")
    {
        solo.setMode(MonitorSolo::Mode::High);
        auto bass = sine(100.0, 0.5f, static_cast<int>(kSr));
        solo.process(bass, false);
        juce::AudioBuffer<float> tail(1, 4800);
        tail.copyFrom(0, 0, bass, 0, bass.getNumSamples() - 4800, 4800);
        CHECK(juce::Decibels::gainToDecibels(rms(tail) / (0.5f / std::sqrt(2.0f))) < -30.0f);

        MonitorSolo low;
        low.prepare(kSr, kBlock);
        low.setMode(MonitorSolo::Mode::Low);
        auto air = sine(8000.0, 0.5f, static_cast<int>(kSr));
        low.process(air, false);
        tail.copyFrom(0, 0, air, 0, air.getNumSamples() - 4800, 4800);
        CHECK(juce::Decibels::gainToDecibels(rms(tail) / (0.5f / std::sqrt(2.0f))) < -30.0f);
    }
    SECTION("switching modes crossfades (no click)")
    {
        const float amplitude = 0.5f;
        auto tone = sine(1000.0, amplitude, static_cast<int>(kSr / 10), true);
        juce::AudioBuffer<float> out;
        out.makeCopyOf(tone);
        // Switch to Mono (L = -R sums to silence) mid-signal.
        juce::AudioBuffer<float> first(out.getArrayOfWritePointers(), 2, 0, 2400);
        solo.process(first, false);
        solo.setMode(MonitorSolo::Mode::Mono);
        juce::AudioBuffer<float> second(out.getArrayOfWritePointers(), 2, 2400, out.getNumSamples() - 2400);
        solo.process(second, false);
        const float sineStep = amplitude * static_cast<float>(2.0 * kPi * 1000.0 / kSr);
        float maxStep = 0.0f;
        const float* d = out.getReadPointer(0);
        for (int i = 1; i < out.getNumSamples(); ++i) maxStep = juce::jmax(maxStep, std::abs(d[i] - d[i - 1]));
        CHECK(maxStep < sineStep * 1.1f + amplitude / 600.0f);
        CHECK(std::abs(d[out.getNumSamples() - 1]) < 1.0e-6f); // and it got there
    }
    SECTION("an offline render passes the plain signal")
    {
        solo.setMode(MonitorSolo::Mode::Sub);
        auto tone = sine(1000.0, 0.5f, kBlock);
        juce::AudioBuffer<float> out;
        out.makeCopyOf(tone);
        solo.process(out, true);
        CHECK(maxAbs(out) == Catch::Approx(maxAbs(tone)));
        CHECK_FALSE(solo.isActive());
    }
}

TEST_CASE("Monitoring solo in the processor: not a parameter, never offline (ENH-10)", "[plugin][enh10][solo]")
{
    Fixture f;
    for (auto* parameter : f.proc.getParameters())
        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*>(parameter)) {
            CHECK_FALSE(ranged->getParameterID().containsIgnoreCase("solo"));
            CHECK_FALSE(ranged->getParameterID().containsIgnoreCase("reference"));
        }
    juce::int64 phase = 0;
    f.proc.setMonitorSolo(MonitorSolo::Mode::Sub);
    auto last = run(f.proc, 1000.0, 0.3f, 0.5, phase);
    CHECK(maxAbs(last) < 0.01f); // 1 kHz is gone
    f.proc.setNonRealtime(true);
    last = run(f.proc, 1000.0, 0.3f, 0.1, phase);
    CHECK(maxAbs(last) > 0.1f); // a bounce hears the whole signal
    f.proc.setNonRealtime(false);
    // Not saved with the project.
    juce::MemoryBlock state;
    f.proc.getStateInformation(state);
    KhensuCopilotProcessor restored;
    restored.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    CHECK(restored.getMonitorSolo() == MonitorSolo::Mode::Off);
}

TEST_CASE("processBlock does not allocate with the reference and a solo on (ENH-10)", "[plugin][enh10][cq03]")
{
    Fixture f;
    auto ref = khensu::copilot::reference::analyseReference(sine(3000.0, 0.5f, static_cast<int>(kSr * 4)), kSr, "r");
    juce::int64 phase = 0;
    run(f.proc, 1000.0, 0.1f, 1.0, phase); // warm-up, unguarded
    f.proc.setReferenceForTesting(ref);
    f.proc.setReferenceListening(true);
    f.proc.setMonitorSolo(MonitorSolo::Mode::Mid);
    khensu::test::resetViolations();
    // The adoption of the new reference, the fades, the match and the band
    // filters all run inside the guard.
    run(f.proc, 1000.0, 0.1f, 2.0, phase, nullptr, true);
    f.proc.setMonitorSolo(MonitorSolo::Mode::Mono);
    f.proc.setReferenceListening(false);
    run(f.proc, 1000.0, 0.1f, 0.5, phase, nullptr, true);
    CHECK(khensu::test::violationCount() == 0);
    khensu::test::resetViolations();
}
