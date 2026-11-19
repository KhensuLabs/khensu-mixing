// tests/plugin/PluginRealtimeStressTest.cpp
// Concurrent real-time stress for the copilot's processBlock (audit AUD-RT-01).
//
// PluginAllocationTest proves steady-state callbacks are allocation-free. This
// file covers the remaining paths a DAW exercises *while audio is running*:
// a dedicated "audio" thread calls processBlock back-to-back with the
// allocation guard armed, while the test (control) thread concurrently
// mutates the hosted chain (add / bypass / wet-mix / remove / clear) and
// automates every corrective parameter. Any heap traffic on the audio thread,
// a crash, or non-finite output fails the test.
#include <catch2/catch_test_macros.hpp>
#include "../../plugin-copilot/Source/PluginProcessor.h"
#include "AllocationGuard.h"
#include "FakeHostedInstances.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>

using khensu::KhensuCopilotProcessor;
using khensu::test::FakeLatencyInstance;
using khensu::test::resetViolations;
using khensu::test::ScopedAllocationGuard;
using khensu::test::violationCount;

namespace {
constexpr double kSr = 48000.0;
constexpr int kBlock = 256;

/// Runs processBlock in a tight loop on its own thread until stopped. The
/// buffers are allocated before the loop starts; inside the loop only the
/// guarded callback runs.
class AudioThreadSimulator {
  public:
    explicit AudioThreadSimulator(KhensuCopilotProcessor& proc) : proc_(proc)
    {
        thread_ = std::thread([this] { run(); });
        while (blocks_.load() < 8) std::this_thread::yield(); // warm up
    }

    ~AudioThreadSimulator() { stop(); }

    void stop()
    {
        running_.store(false);
        if (thread_.joinable()) thread_.join();
    }

    long blocks() const { return blocks_.load(); }
    bool sawNonFinite() const { return nonFinite_.load(); }

  private:
    void run()
    {
        juce::AudioBuffer<float> buffer(2, kBlock);
        juce::MidiBuffer midi;
        juce::Random rng(7);
        long count = 0;
        while (running_.load()) {
            for (int c = 0; c < 2; ++c) {
                float* d = buffer.getWritePointer(c);
                for (int i = 0; i < kBlock; ++i) d[i] = rng.nextFloat() * 0.5f - 0.25f;
            }
            if (count >= 8) {
                ScopedAllocationGuard guard;
                proc_.processBlock(buffer, midi);
            } else {
                proc_.processBlock(buffer, midi); // first blocks may build DSP state
            }
            for (int c = 0; c < 2; ++c) {
                const float* d = buffer.getReadPointer(c);
                for (int i = 0; i < kBlock; ++i)
                    if (!std::isfinite(d[i])) nonFinite_.store(true);
            }
            blocks_.store(++count);
        }
    }

    KhensuCopilotProcessor& proc_;
    std::atomic<bool> running_{true};
    std::atomic<long> blocks_{0};
    std::atomic<bool> nonFinite_{false};
    std::thread thread_;
};

void waitForBlocks(const AudioThreadSimulator& audio, long extra)
{
    const long target = audio.blocks() + extra;
    while (audio.blocks() < target) std::this_thread::yield();
}

std::unique_ptr<FakeLatencyInstance> preparedFake(int latency)
{
    auto fake = std::make_unique<FakeLatencyInstance>(latency);
    fake->prepareToPlay(kSr, kBlock); // allocate on the control thread
    return fake;
}
} // namespace

TEST_CASE("Hosted-chain edits during playback keep processBlock allocation-free",
          "[plugin][audrt01][stress]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    KhensuCopilotProcessor proc;
    proc.setPlayConfigDetails(2, 2, kSr, kBlock);
    proc.prepareToPlay(kSr, kBlock);

    resetViolations();
    AudioThreadSimulator audio(proc);

    for (int round = 0; round < 12; ++round) {
        proc.addPluginInstanceToChain(preparedFake(64 + round * 16), "FakeA");
        waitForBlocks(audio, 3);
        proc.addPluginInstanceToChain(preparedFake(128), "FakeB");
        waitForBlocks(audio, 3);
        proc.setHostedPluginWetMix(0, 0.25f + 0.05f * static_cast<float>(round % 10));
        waitForBlocks(audio, 2);
        proc.setHostedPluginBypass(1, (round % 2) == 0);
        waitForBlocks(audio, 2);
        proc.removePluginFromChain(0);
        waitForBlocks(audio, 3);
        if (round % 3 == 0)
            proc.clearHostedPluginChain();
        waitForBlocks(audio, 2);
    }
    proc.clearHostedPluginChain();
    waitForBlocks(audio, 4);
    audio.stop();

    CHECK_FALSE(audio.sawNonFinite());
    REQUIRE(violationCount() == 0);
}

TEST_CASE("Concurrent parameter automation and chain edits stay real-time safe",
          "[plugin][audrt01][stress]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    KhensuCopilotProcessor proc;
    proc.setPlayConfigDetails(2, 2, kSr, kBlock);
    proc.prepareToPlay(kSr, kBlock);
    for (const char* id : {KhensuCopilotProcessor::ParamIDs::hpfOn, KhensuCopilotProcessor::ParamIDs::eqOn,
                           KhensuCopilotProcessor::ParamIDs::compOn, KhensuCopilotProcessor::ParamIDs::limOn})
        proc.setParamThreadSafe(id, 1.0f);

    resetViolations();
    AudioThreadSimulator audio(proc);

    // A second control thread automates parameters at host-automation rate
    // while this thread edits the hosted chain: three threads touch the
    // processor at once, as in a DAW with automation lanes playing.
    std::atomic<bool> automate{true};
    std::thread automation([&proc, &automate] {
        int step = 0;
        while (automate.load()) {
            const auto s = static_cast<float>(step % 16);
            proc.setParamThreadSafe(KhensuCopilotProcessor::ParamIDs::hpfFreq, 40.0f + s * 20.0f);
            proc.setParamThreadSafe(KhensuCopilotProcessor::ParamIDs::eqFreq, 300.0f + s * 400.0f);
            proc.setParamThreadSafe(KhensuCopilotProcessor::ParamIDs::eqGainDb, -8.0f + s);
            proc.setParamThreadSafe(KhensuCopilotProcessor::ParamIDs::compThreshDb, -30.0f + s);
            proc.setParamThreadSafe(KhensuCopilotProcessor::ParamIDs::compMultiband, (step / 8) % 2 ? 1.0f : 0.0f);
            proc.setParamThreadSafe(KhensuCopilotProcessor::ParamIDs::outGainDb, -3.0f + s * 0.25f);
            proc.setAbMode(step % 3);
            ++step;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    for (int round = 0; round < 8; ++round) {
        proc.addPluginInstanceToChain(preparedFake(32 * (round + 1)), "Fake");
        waitForBlocks(audio, 4);
        proc.setHostedPluginWetMix(0, 0.5f);
        waitForBlocks(audio, 4);
        proc.removePluginFromChain(0);
        waitForBlocks(audio, 4);
    }

    automate.store(false);
    automation.join();
    waitForBlocks(audio, 4);
    audio.stop();

    CHECK(audio.blocks() > 100);
    CHECK_FALSE(audio.sawNonFinite());
    REQUIRE(violationCount() == 0);
}

TEST_CASE("setNonRealtime + processBlock never wait on the hosted-chain control lock", "[plugin][audit004]")
{
    // The JUCE VST3 wrapper calls setNonRealtime() from the audio callback on
    // every process call. Hold the chain manager's control lock on another
    // thread (a heavy hosted plugin's getStateInformation during an autosave)
    // and drive the wrapper's sequence: neither call may block on it.
    juce::ScopedJuceInitialiser_GUI juceInit;
    KhensuCopilotProcessor proc;
    proc.setPlayConfigDetails(2, 2, kSr, kBlock);
    proc.prepareToPlay(kSr, kBlock);
    auto slow = std::make_unique<khensu::test::SlowStateInstance>(800);
    auto* raw = slow.get();
    proc.addPluginInstanceToChain(std::move(slow), "SlowState");

    std::thread saver([&proc] {
        juce::MemoryBlock state;
        proc.getStateInformation(state); // holds the control lock ~800 ms
    });
    while (!raw->insideState.load()) std::this_thread::yield();

    juce::AudioBuffer<float> buffer(2, kBlock);
    juce::MidiBuffer midi;
    const auto start = std::chrono::steady_clock::now();
    for (int block = 0; block < 64; ++block) {
        buffer.clear();
        proc.setNonRealtime((block / 8) % 2 == 1); // toggles, as an offline bounce starts/stops
        proc.processBlock(buffer, midi);
    }
    const auto elapsedMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    const bool lockStillHeld = raw->insideState.load();
    saver.join();

    INFO("64 blocks took " << elapsedMs << " ms");
    REQUIRE(lockStillHeld); // the loop really ran while the lock was held
    CHECK(elapsedMs < 400);

    // The flag still reaches the hosted instance, from the audio path.
    proc.setNonRealtime(true);
    proc.processBlock(buffer, midi);
    CHECK(raw->isNonRealtime());
    proc.setNonRealtime(false);
    proc.processBlock(buffer, midi);
    CHECK_FALSE(raw->isNonRealtime());
}

TEST_CASE("A hosted plugin emitting NaN is contained and processing recovers", "[plugin][audit025]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    KhensuCopilotProcessor proc;
    proc.setPlayConfigDetails(2, 2, kSr, kBlock);
    proc.prepareToPlay(kSr, kBlock);
    proc.setParamThreadSafe(KhensuCopilotProcessor::ParamIDs::eqOn, 1.0f);
    proc.setParamThreadSafe(KhensuCopilotProcessor::ParamIDs::eqGainDb, 3.0f);
    proc.setParamThreadSafe(KhensuCopilotProcessor::ParamIDs::limOn, 1.0f);
    auto nan = std::make_unique<khensu::test::NanInstance>();
    auto* raw = nan.get();
    proc.addPluginInstanceToChain(std::move(nan), "NaN");

    juce::AudioBuffer<float> buffer(2, kBlock);
    juce::MidiBuffer midi;
    const auto fill = [&buffer] {
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < kBlock; ++i) buffer.setSample(c, i, 0.25f * std::sin(0.05f * static_cast<float>(i)));
    };
    const auto allFinite = [&buffer] {
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < kBlock; ++i)
                if (!std::isfinite(buffer.getSample(c, i))) return false;
        return true;
    };

    for (int block = 0; block < 8; ++block) {
        fill();
        proc.processBlock(buffer, midi);
        CHECK(allFinite()); // nothing non-finite ever reaches the host
    }
    CHECK(std::isfinite(proc.getMomentaryLufs()));

    raw->emitNan.store(false);
    for (int block = 0; block < 40; ++block) {
        fill();
        proc.processBlock(buffer, midi);
        REQUIRE(allFinite());
    }
    // Signal flows again: the poisoned filter / limiter states were reset.
    CHECK(buffer.getMagnitude(0, 0, kBlock) > 0.05f);
    CHECK(proc.getMomentaryLufs() > -60.0f);
}
