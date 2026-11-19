// tests/plugin/PluginAllocationTest.cpp
// Steady-state processBlock must not touch the heap (audit CQ-03). The guard
// is thread_local, so only the thread calling processBlock is watched.
//
// Filter-coefficient construction is delegated to CopilotCorrectiveDsp's
// control worker. The immutable hosted-chain handoff below must likewise not
// acquire shared ownership in processBlock.
#include <catch2/catch_test_macros.hpp>
#include "../../plugin-copilot/Source/PluginProcessor.h"

#include <limits>
#include "AllocationGuard.h"
#include "FakeHostedInstances.h"

using khensu::KhensuCopilotProcessor;
using khensu::test::FakeLatencyInstance;
using khensu::test::resetViolations;
using khensu::test::ScopedAllocationGuard;
using khensu::test::violationCount;

namespace {
constexpr double kSr = 48000.0;
constexpr int kBlock = 512;

/// `guarded` arms the allocation guard around each processBlock call only: the
/// test's own buffers are allocated (by malloc, which the guard sees since
/// VER-5) before it is armed.
void runBlocks(KhensuCopilotProcessor& proc, int blocks, bool withSignal, bool guarded = false)
{
    juce::AudioBuffer<float> buf(2, kBlock);
    juce::MidiBuffer midi;
    juce::Random rng(42);
    for (int b = 0; b < blocks; ++b) {
        if (withSignal) {
            for (int c = 0; c < 2; ++c) {
                float* d = buf.getWritePointer(c);
                for (int i = 0; i < kBlock; ++i) d[i] = rng.nextFloat() * 0.5f - 0.25f;
            }
        } else {
            buf.clear();
        }
        if (guarded) {
            ScopedAllocationGuard guard;
            proc.processBlock(buf, midi);
        } else {
            proc.processBlock(buf, midi);
        }
    }
}
} // namespace

TEST_CASE("The allocation guard itself detects allocations", "[plugin][cq03]")
{
    // Without this, a broken guard (never armed, overrides not linked) would
    // make every zero-violation assertion below vacuous.
    resetViolations();
    {
        ScopedAllocationGuard guard;
        auto* p = new int(7);
        delete p;
    }
    REQUIRE(violationCount() >= 1);
    resetViolations();
}

TEST_CASE("The allocation guard sees malloc, not only operator new", "[plugin][cq03][ver5]")
{
    // VER-5: JUCE's HeapBlock (AudioBuffer, Array and MidiBuffer growth) uses
    // malloc/calloc/realloc directly. A guard that only replaced operator new
    // could not see any of it.
#if defined(__SANITIZE_ADDRESS__)
    // ASan replaces the CRT allocator with its own, which never reaches the
    // HeapAlloc hook the guard counts (VER-2), so the guard is blind to malloc
    // in that build. The normal builds keep this check.
    SKIP("AddressSanitizer build: malloc bypasses the guard's HeapAlloc hook");
#else
    resetViolations();
    {
        ScopedAllocationGuard guard;
        juce::HeapBlock<float> block;
        block.malloc(256);
        block.realloc(1024);
        block.calloc(64);
    }
    CHECK(violationCount() >= 3);
    resetViolations();
#endif
}

namespace {
void fill(juce::AudioBuffer<float>& buf, juce::Random& rng, float amplitude)
{
    for (int c = 0; c < buf.getNumChannels(); ++c) {
        float* d = buf.getWritePointer(c);
        for (int i = 0; i < buf.getNumSamples(); ++i) d[i] = (rng.nextFloat() * 2.0f - 1.0f) * amplitude;
    }
}

/// What a host's automation does: the parameter's own setValue, on the audio
/// thread, inside the callback. (setParamThreadSafe is the transport worker's
/// path and posts a message, so it is not called from processBlock's thread.)
void hostAutomate(KhensuCopilotProcessor& proc, const char* id, float plain)
{
    auto* parameter = proc.getAPVTS().getParameter(id);
    parameter->setValue(parameter->convertTo0to1(plain));
}

/// Warm up outside the guard, then count allocations over 32 guarded blocks.
template <typename Process>
long guardedBlocks(int channels, float amplitude, Process&& process)
{
    juce::AudioBuffer<float> buf(channels, kBlock);
    juce::MidiBuffer midi;
    juce::Random rng(7);
    for (int b = 0; b < 4; ++b) {
        fill(buf, rng, amplitude);
        process(buf, midi, b);
    }
    resetViolations();
    for (int b = 0; b < 32; ++b) {
        fill(buf, rng, amplitude);
        ScopedAllocationGuard guard;
        process(buf, midi, b);
    }
    return violationCount();
}
} // namespace

TEST_CASE("processBlock does not allocate in the paths VER-5 found unguarded", "[plugin][cq03][ver5]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    KhensuCopilotProcessor proc;

    SECTION("mono")
    {
        proc.setPlayConfigDetails(1, 1, kSr, kBlock);
        proc.prepareToPlay(kSr, kBlock);
        CHECK(guardedBlocks(1, 0.5f, [&](auto& b, auto& m, int) { proc.processBlock(b, m); }) == 0);
    }

    proc.setPlayConfigDetails(2, 2, kSr, kBlock);
    proc.prepareToPlay(kSr, kBlock);

    SECTION("limiter engaged")
    {
        proc.setParamThreadSafe(KhensuCopilotProcessor::ParamIDs::limOn, 1.0f);
        proc.setParamThreadSafe(KhensuCopilotProcessor::ParamIDs::limThreshDb, -12.0f);
        proc.setParamThreadSafe(KhensuCopilotProcessor::ParamIDs::outGainDb, 6.0f);
        CHECK(guardedBlocks(2, 0.9f, [&](auto& b, auto& m, int) { proc.processBlock(b, m); }) == 0);
    }

    SECTION("A/B Original with the limiter capping the match gain")
    {
        proc.setParamThreadSafe(KhensuCopilotProcessor::ParamIDs::limOn, 1.0f);
        proc.setAbMode(1);
        CHECK(guardedBlocks(2, 0.9f, [&](auto& b, auto& m, int) { proc.processBlock(b, m); }) == 0);
    }

    SECTION("Blind, reshuffled every few blocks")
    {
        proc.setAbMode(3);
        CHECK(guardedBlocks(2, 0.5f,
                            [&](auto& b, auto& m, int i) {
                                if (i % 5 == 0) proc.nextBlindTrial();
                                proc.processBlock(b, m);
                            })
              == 0);
    }

    SECTION("processBlockBypassed, and bypass toggled mid-run")
    {
        CHECK(guardedBlocks(2, 0.5f, [&](auto& b, auto& m, int) { proc.processBlockBypassed(b, m); }) == 0);
        CHECK(guardedBlocks(2, 0.5f,
                            [&](auto& b, auto& m, int i) {
                                hostAutomate(proc, KhensuCopilotProcessor::ParamIDs::bypass,
                                             (i / 4) % 2 == 0 ? 1.0f : 0.0f);
                                proc.processBlock(b, m);
                            })
              == 0);
    }

    SECTION("NaN and infinite input")
    {
        CHECK(guardedBlocks(2, 0.5f,
                            [&](auto& b, auto& m, int i) {
                                b.setSample(0, i % kBlock, std::numeric_limits<float>::quiet_NaN());
                                b.setSample(1, (i * 7) % kBlock, std::numeric_limits<float>::infinity());
                                proc.processBlock(b, m);
                            })
              == 0);
    }

    SECTION("automation arriving inside the guarded callback")
    {
        // A host writes parameters from the audio thread, inside the callback.
        proc.setParamThreadSafe(KhensuCopilotProcessor::ParamIDs::compOn, 1.0f);
        proc.setParamThreadSafe(KhensuCopilotProcessor::ParamIDs::eqOn, 1.0f);
        CHECK(guardedBlocks(2, 0.5f,
                            [&](auto& b, auto& m, int i) {
                                hostAutomate(proc, KhensuCopilotProcessor::ParamIDs::eqGainDb,
                                             -6.0f + static_cast<float>(i % 13));
                                hostAutomate(proc, KhensuCopilotProcessor::ParamIDs::compThreshDb,
                                             -30.0f + static_cast<float>(i % 20));
                                hostAutomate(proc, KhensuCopilotProcessor::ParamIDs::chainMix,
                                             static_cast<float>(i % 10) / 10.0f);
                                proc.processBlock(b, m);
                            })
              == 0);
    }
}

TEST_CASE("Steady-state processBlock does not allocate", "[plugin][cq03]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    KhensuCopilotProcessor proc;
    proc.setPlayConfigDetails(2, 2, kSr, kBlock);
    proc.prepareToPlay(kSr, kBlock);

    SECTION("built-in chain, defaults")
    {
        runBlocks(proc, 4, true); // warmup: first blocks may rebuild coefficients
        resetViolations();
        {
            runBlocks(proc, 32, true, true);
        }
        REQUIRE(violationCount() == 0);
    }

    SECTION("with A/B Delta active")
    {
        proc.setAbMode(2);
        runBlocks(proc, 4, true);
        resetViolations();
        {
            runBlocks(proc, 32, true, true);
        }
        REQUIRE(violationCount() == 0);
    }

    SECTION("with a partial-wet latency-bearing hosted plugin")
    {
        auto fake = std::make_unique<FakeLatencyInstance>(300);
        fake->prepareToPlay(kSr, kBlock);
        proc.addPluginInstanceToChain(std::move(fake), "FakeLatency");
        proc.setHostedPluginWetMix(0, 0.5f);
        runBlocks(proc, 4, true);
        resetViolations();
        {
            runBlocks(proc, 32, true, true);
        }
        REQUIRE(violationCount() == 0);
    }
}

TEST_CASE("Automated corrective parameters do not allocate in processBlock", "[plugin][cxx02]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    KhensuCopilotProcessor proc;
    proc.setPlayConfigDetails(2, 2, kSr, kBlock);
    proc.prepareToPlay(kSr, kBlock);

    juce::AudioBuffer<float> buffer(2, kBlock);
    juce::MidiBuffer midi;
    proc.setParamThreadSafe(KhensuCopilotProcessor::ParamIDs::hpfOn, 1.0f);
    proc.setParamThreadSafe(KhensuCopilotProcessor::ParamIDs::eqOn, 1.0f);
    runBlocks(proc, 4, true); // Allow worker publication and DSP warmup.

    resetViolations();
    for (int block = 0; block < 32; ++block) {
        // Deliberately mutate the atomics between callbacks, as a host's
        // parameter automation does. The corrective DSP re-derives its
        // filter coefficients on the audio thread while a parameter
        // smooths, so every block below recomputes them.
        proc.setParamThreadSafe(KhensuCopilotProcessor::ParamIDs::hpfFreq,
                                80.0f + static_cast<float>((block % 8) * 100));
        proc.setParamThreadSafe(KhensuCopilotProcessor::ParamIDs::eqFreq,
                                600.0f + static_cast<float>((block % 6) * 350));
        proc.setParamThreadSafe(KhensuCopilotProcessor::ParamIDs::eqGainDb,
                                -6.0f + static_cast<float>(block % 13));
        proc.setParamThreadSafe(KhensuCopilotProcessor::ParamIDs::eqQ,
                                0.7f + static_cast<float>(block % 5) * 0.4f);

        for (int channel = 0; channel < buffer.getNumChannels(); ++channel) {
            float* samples = buffer.getWritePointer(channel);
            for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
                samples[sample] = 0.125f;
        }

        ScopedAllocationGuard guard;
        proc.processBlock(buffer, midi);
    }

    REQUIRE(violationCount() == 0);
}

TEST_CASE("Oversized multiband callback is split and processed without allocation", "[plugin][cxx01][audit027]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    KhensuCopilotProcessor proc;
    proc.setPlayConfigDetails(2, 2, kSr, kBlock);
    proc.prepareToPlay(kSr, kBlock);
    proc.setParamThreadSafe(KhensuCopilotProcessor::ParamIDs::compOn, 1.0f);
    proc.setParamThreadSafe(KhensuCopilotProcessor::ParamIDs::compMultiband, 1.0f);

    // Let parameter smoothing and coefficient setup settle outside the
    // allocation guard. The guarded callback then exercises the previously
    // resizing path with a host block larger than prepareToPlay advertised.
    runBlocks(proc, 4, true);
    juce::AudioBuffer<float> oversized(2, kBlock * 2);
    for (int channel = 0; channel < oversized.getNumChannels(); ++channel)
        for (int sample = 0; sample < oversized.getNumSamples(); ++sample)
            oversized.setSample(channel, sample, 0.25f);
    juce::MidiBuffer midi;

    resetViolations();
    {
        ScopedAllocationGuard guard;
        proc.processBlock(oversized, midi);
    }

    REQUIRE(violationCount() == 0);
}

TEST_CASE("An oversized host block still runs the corrective chain on every sample", "[plugin][audit027]")
{
    // AUDIT-027: the oversized block used to skip corrective DSP entirely
    // (and the A/B / bypass alignment), so its level jumped. The processor now
    // splits it into prepared-size chunks.
    juce::ScopedJuceInitialiser_GUI juceInit;
    KhensuCopilotProcessor proc;
    proc.setPlayConfigDetails(2, 2, kSr, kBlock);
    proc.prepareToPlay(kSr, kBlock);
    proc.setParamThreadSafe(KhensuCopilotProcessor::ParamIDs::hpfOn, 1.0f);
    proc.setParamThreadSafe(KhensuCopilotProcessor::ParamIDs::hpfFreq, 1000.0f);

    juce::AudioBuffer<float> oversized(2, kBlock * 3);
    for (int channel = 0; channel < 2; ++channel)
        for (int sample = 0; sample < oversized.getNumSamples(); ++sample)
            oversized.setSample(channel, sample, 1.0f); // DC: the HPF removes it
    juce::MidiBuffer midi;
    proc.processBlock(oversized, midi);

    // The tail of the oversized block went through the high-pass too.
    CHECK(std::abs(oversized.getSample(0, oversized.getNumSamples() - 1)) < 0.05f);
    CHECK(std::abs(oversized.getSample(1, oversized.getNumSamples() - 1)) < 0.05f);
}

TEST_CASE("Filter automation does not allocate from processBlock", "[plugin][cxx02]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    KhensuCopilotProcessor proc;
    proc.setPlayConfigDetails(2, 2, kSr, kBlock);
    proc.prepareToPlay(kSr, kBlock);

    // Parameter writes happen before the guarded callback. The coefficient
    // worker builds a new snapshot independently; processBlock must only
    // adopt its already-built numeric values.
    proc.setParamThreadSafe(KhensuCopilotProcessor::ParamIDs::hpfFreq, 160.0f);
    proc.setParamThreadSafe(KhensuCopilotProcessor::ParamIDs::lpfFreq, 14000.0f);
    proc.setParamThreadSafe(KhensuCopilotProcessor::ParamIDs::eqFreq, 2400.0f);
    proc.setParamThreadSafe(KhensuCopilotProcessor::ParamIDs::eqGainDb, -4.0f);
    proc.setParamThreadSafe(KhensuCopilotProcessor::ParamIDs::eqQ, 1.4f);
    proc.setParamThreadSafe(KhensuCopilotProcessor::ParamIDs::eq2Freq, 480.0f);
    proc.setParamThreadSafe(KhensuCopilotProcessor::ParamIDs::eq3Freq, 7200.0f);

    resetViolations();
    {
        runBlocks(proc, 32, true, true);
    }
    REQUIRE(violationCount() == 0);
}

TEST_CASE("Filter automation reaches the corrective chain on the audio thread", "[plugin][cxx02][plg08]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    KhensuCopilotProcessor proc;
    proc.setPlayConfigDetails(2, 2, kSr, kBlock);
    proc.prepareToPlay(kSr, kBlock);
    proc.setParamThreadSafe(KhensuCopilotProcessor::ParamIDs::hpfOn, 1.0f);
    proc.setParamThreadSafe(KhensuCopilotProcessor::ParamIDs::hpfFreq, 1000.0f);

    // Coefficients are computed per sub-block in audio time (PLG-08): the
    // 20 ms frequency ramp and the 10 ms stage fade-in (PLG-14) complete
    // within the first blocks of audio, no wall-clock wait involved.
    juce::AudioBuffer<float> buffer(2, kBlock);
    juce::MidiBuffer midi;
    for (int warm = 0; warm < 4; ++warm) {
        for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
            for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
                buffer.setSample(channel, sample, 1.0f);
        proc.processBlock(buffer, midi);
    }

    CHECK(std::abs(buffer.getSample(0, kBlock - 1)) < 0.05f);
}
