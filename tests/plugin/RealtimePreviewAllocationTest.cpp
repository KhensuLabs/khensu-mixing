// tests/plugin/RealtimePreviewAllocationTest.cpp
// STU-5 / STU-9: the shared real-time pieces the desktop transport runs on
// its audio thread - the monitoring stage (dsp::MonitorSolo) and the live
// preview chain (dsp::RealtimeChain) - never touch the heap while
// processing. Lives here because only this binary carries the allocation
// guard (AllocationGuard.cpp replaces operator new binary-wide).
#include <catch2/catch_test_macros.hpp>

#include <juce_audio_basics/juce_audio_basics.h>
#include <khensu/dsp/MonitorSolo.h>
#include <khensu/dsp/RealtimeChain.h>

#include "AllocationGuard.h"

using khensu::test::resetViolations;
using khensu::test::ScopedAllocationGuard;
using khensu::test::violationCount;
namespace kd = khensu::dsp;

namespace {
constexpr double kSr = 48000.0;

void fillNoise(juce::AudioBuffer<float>& buf, juce::Random& rng)
{
    for (int c = 0; c < buf.getNumChannels(); ++c) {
        float* d = buf.getWritePointer(c);
        for (int i = 0; i < buf.getNumSamples(); ++i) d[i] = rng.nextFloat() * 0.8f - 0.4f;
    }
}
} // namespace

TEST_CASE("Live preview chain processes without allocating (STU-9)", "[stu9][stu18][alloc]")
{
    const std::vector<kd::ChainStageSpec> chain = {
        {"HighpassFilter", {{"cutoff_frequency_hz", 40.0f}, {"slope_db_per_oct", 24.0f}}},
        {"PeakFilter", {{"cutoff_frequency_hz", 1500.0f}, {"gain_db", -4.0f}}},
        {"HighShelfFilter", {{"cutoff_frequency_hz", 8000.0f}, {"gain_db", 2.0f}}},
        {"Compressor", {{"threshold_db", -20.0f}, {"ratio", 3.0f}}},
        {"TransientShaper", {{"attack_gain_db", 2.0f}}},
        {"StereoWidener", {{"width", 1.2f}}},
        {"MidSideProcessor", {{"side_eq_gain_db", -2.0f}, {"side_highpass_frequency_hz", 100.0f}}},
        {"NoiseGate", {{"threshold_db", -60.0f}}},
        // STU-18 stand-ins (thresholds low enough that the dynamic bands
        // move, re-designing their coefficients on the audio thread).
        {"DeEsser", {{"threshold_db", -40.0f}}},
        {"DynamicEQ", {{"frequency_hz", 250.0f}, {"threshold_db", -40.0f}}},
        {"DynamicEQ", {{"frequency_hz", 6000.0f}, {"shape", 2.0f}, {"threshold_db", -40.0f}}},
        {"BassMonoMaker", {{"crossover_hz", 150.0f}}},
        {"TapeSaturation", {{"drive_db", 9.0f}}},
        {"Exciter", {{"frequency_hz", 4000.0f}}},
        {"MultibandCompressor", {}}, // skipped: must not run (it allocates)
        {"Gain", {{"gain_db", 3.0f}}},
        {"Limiter", {{"threshold_db", -1.0f}}}};
    kd::RealtimeChain live; // loudness match on
    std::string error;
    REQUIRE(live.prepare(chain, kSr, 2, 1024, error));
    kd::RealtimeChain next;
    REQUIRE(next.prepare(chain, kSr, 2, 1024, error));

    juce::Random rng(7);
    juce::AudioBuffer<float> buf(2, 4096);
    resetViolations();
    // Device blocks below, at and above the chain's internal block.
    for (const int block : {64, 512, 1024, 1500, 4096}) {
        for (int k = 0; k < 20; ++k) {
            fillNoise(buf, rng);
            float* ptrs[2] = {buf.getWritePointer(0), buf.getWritePointer(1)};
            ScopedAllocationGuard guard;
            live.process(ptrs, 2, block);
        }
    }
    {
        ScopedAllocationGuard guard;
        next.adoptMatchState(live); // the audio thread's hand-over
        next.resetMatch();
    }
    CHECK(violationCount() == 0);
}

TEST_CASE("Monitoring stage processes without allocating (STU-5)", "[stu5][alloc]")
{
    kd::MonitorSolo solo;
    solo.prepare(kSr, 512);
    juce::Random rng(11);
    juce::AudioBuffer<float> buf(2, 512);
    resetViolations();
    using Mode = kd::MonitorSolo::Mode;
    for (const auto mode : {Mode::Mono, Mode::Side, Mode::Sub, Mode::Off, Mode::Sub}) {
        solo.setMode(mode);
        solo.setDim(mode == Mode::Sub);
        for (int k = 0; k < 10; ++k) {
            fillNoise(buf, rng);
            ScopedAllocationGuard guard;
            solo.process(buf, 0, 512, false);
        }
    }
    CHECK(violationCount() == 0);
}
