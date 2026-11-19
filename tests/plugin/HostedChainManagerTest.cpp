// tests/plugin/HostedChainManagerTest.cpp
// HostedPluginChainManager contracts with third-party instances (audits M3,
// M4, M5, M6, M7, L8, L9): layout before prepare, re-prepare at the current
// rate on arrival, host blocks split to the prepared size, per-slot MIDI,
// playhead / offline forwarding, placeholders, synchronous release.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "../../plugin-copilot/Source/HostedPluginChainManager.h"
#include "FakeHostedInstances.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>

using khensu::HostedPluginChainManager;
using khensu::test::DtorCountingInstance;
using khensu::test::RecordingInstance;
using khensu::test::SidechainInstance;

namespace {
int lastIndexOf(const std::vector<std::string>& events, const std::string& what)
{
    for (int i = static_cast<int>(events.size()) - 1; i >= 0; --i)
        if (events[static_cast<size_t>(i)] == what) return i;
    return -1;
}

struct FixedPlayHead final : juce::AudioPlayHead {
    juce::Optional<PositionInfo> getPosition() const override { return PositionInfo{}; }
};
} // namespace

TEST_CASE("Hosted instances get their bus layout before prepareToPlay", "[plugin][hosted][m3]")
{
    HostedPluginChainManager manager;
    manager.prepareToPlay(48000.0, 256, 1); // mono session

    auto instance = std::make_unique<RecordingInstance>();
    auto* raw = instance.get();
    manager.addPluginInstance(std::move(instance), "Rec");

    CHECK(raw->getMainBusNumInputChannels() == 1);
    CHECK(raw->getMainBusNumOutputChannels() == 1);
    const int layout = lastIndexOf(raw->events, "layout");
    const int prepare = lastIndexOf(raw->events, "prepare");
    REQUIRE(layout >= 0);
    REQUIRE(prepare >= 0);
    CHECK(layout < prepare);
}

TEST_CASE("An instance created at a stale rate is re-prepared at the current one", "[plugin][hosted][m7]")
{
    HostedPluginChainManager manager;
    manager.prepareToPlay(96000.0, 128, 2);

    // An async load captured 44.1k/1024 at request time.
    auto instance = std::make_unique<RecordingInstance>();
    auto* raw = instance.get();
    raw->setRateAndBufferSizeDetails(44100.0, 1024);
    raw->prepareToPlay(44100.0, 1024);

    manager.addPluginInstance(std::move(instance), "Rec");
    CHECK(raw->lastPrepareRate == Catch::Approx(96000.0));
    CHECK(raw->lastPrepareBlock == 128);
    CHECK(raw->getSampleRate() == Catch::Approx(96000.0));
    CHECK(raw->getBlockSize() == 128);

    // A later host re-prepare reaches it as well.
    manager.prepareToPlay(48000.0, 64, 2);
    CHECK(raw->lastPrepareRate == Catch::Approx(48000.0));
    CHECK(raw->lastPrepareBlock == 64);
}

TEST_CASE("Host blocks larger than the prepared size are split", "[plugin][hosted][m4]")
{
    HostedPluginChainManager manager;
    manager.prepareToPlay(48000.0, 64, 2);

    auto full = std::make_unique<RecordingInstance>();
    auto* fullRaw = full.get();
    auto partial = std::make_unique<RecordingInstance>();
    auto* partialRaw = partial.get();
    manager.addPluginInstance(std::move(full), "Full");
    manager.addPluginInstance(std::move(partial), "Partial", {}, false, 0.5f);

    juce::AudioBuffer<float> buffer(2, 200);
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < 200; ++i) buffer.setSample(c, i, std::sin(0.01f * static_cast<float>(i + 1 + c)));
    juce::AudioBuffer<float> original(buffer);

    manager.processBlock(buffer, false);

    CHECK(fullRaw->maxBlockSeen == 64);
    CHECK(fullRaw->totalSamplesSeen == 200);
    CHECK(partialRaw->maxBlockSeen == 64);
    CHECK(partialRaw->totalSamplesSeen == 200);
    // Unity passthrough at 50 % wet is still unity, across sub-block seams.
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < 200; ++i)
            CHECK(buffer.getSample(c, i) == Catch::Approx(original.getSample(c, i)).margin(1.0e-6));
}

TEST_CASE("One slot's MIDI output never reaches the next slot", "[plugin][hosted][l8]")
{
    HostedPluginChainManager manager;
    manager.prepareToPlay(48000.0, 128, 2);

    auto emitter = std::make_unique<RecordingInstance>(true);
    auto* emitterRaw = emitter.get();
    auto listener = std::make_unique<RecordingInstance>();
    auto* listenerRaw = listener.get();
    manager.addPluginInstance(std::move(emitter), "Emitter");
    manager.addPluginInstance(std::move(listener), "Listener");

    juce::AudioBuffer<float> buffer(2, 128);
    buffer.clear();
    manager.processBlock(buffer, false);
    manager.processBlock(buffer, false);

    CHECK(listenerRaw->midiEventsSeen == 0);
    CHECK(emitterRaw->midiEventsSeen == 0); // its own output is not fed back next block
}

TEST_CASE("Hosted instances receive the host playhead and offline flag", "[plugin][hosted][l9]")
{
    HostedPluginChainManager manager;
    manager.prepareToPlay(48000.0, 128, 2, true);

    auto instance = std::make_unique<RecordingInstance>();
    auto* raw = instance.get();
    manager.addPluginInstance(std::move(instance), "Rec");
    CHECK(raw->isNonRealtime()); // a slot added during an offline render inherits it

    // AUDIT-004: setNonRealtime only records the flag (lock-free, it is
    // called from the audio callback); the next processBlock applies it.
    FixedPlayHead playHead;
    juce::AudioBuffer<float> buffer(2, 128);
    buffer.clear();
    manager.setNonRealtime(false);
    manager.processBlock(buffer, false, &playHead);
    CHECK_FALSE(raw->isNonRealtime());
    manager.setNonRealtime(true);
    manager.processBlock(buffer, false, &playHead);
    CHECK(raw->isNonRealtime());
    CHECK(raw->getPlayHead() == &playHead);
}

TEST_CASE("Placeholders keep an unrestorable slot's position, controls and state", "[plugin][hosted][m5]")
{
    HostedPluginChainManager manager;
    manager.prepareToPlay(48000.0, 128, 2);

    manager.addPluginInstance(std::make_unique<RecordingInstance>(), "Live", "C:/plugins/live.vst3");
    HostedPluginChainManager::PersistedSlot saved;
    saved.name = "Gone";
    saved.filePath = "C:/plugins/gone.vst3";
    saved.bypassed = true;
    saved.wetMix = 0.25f;
    constexpr char blob[] = "opaque-gone-state";
    saved.pluginState = juce::MemoryBlock(blob, sizeof(blob));
    manager.addPlaceholder(saved);

    const auto slots = manager.getSlotInfo();
    REQUIRE(slots.size() == 2);
    CHECK_FALSE(slots[0].missing);
    CHECK(slots[1].missing);
    CHECK(slots[1].name == "Gone");
    CHECK(slots[0].id != slots[1].id);
    CHECK(manager.getActiveLatency() == 0);

    // The audio path skips the placeholder.
    juce::AudioBuffer<float> buffer(2, 128);
    buffer.clear();
    manager.processBlock(buffer, false);

    const auto state = manager.captureState();
    REQUIRE(state.size() == 2);
    CHECK(state[1].name == "Gone");
    CHECK(state[1].filePath == "C:/plugins/gone.vst3");
    CHECK(state[1].bypassed);
    CHECK(state[1].wetMix == Catch::Approx(0.25f));
    CHECK(state[1].pluginState == saved.pluginState);

    manager.removePlugin(1);
    CHECK(manager.getNumPlugins() == 1);
}

TEST_CASE("Destroying the manager on the message thread releases instances synchronously", "[plugin][hosted][m6]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    std::atomic<int> destroyed{0};
    {
        HostedPluginChainManager manager;
        manager.prepareToPlay(48000.0, 128, 2);
        manager.addPluginInstance(std::make_unique<DtorCountingInstance>(destroyed), "A");
        manager.addPluginInstance(std::make_unique<DtorCountingInstance>(destroyed), "B");
        manager.removePlugin(0); // retired, not yet reaped
    }
    // No dispatch loop has run: nothing may be left to a callAsync that could
    // execute after the module is unloaded.
    CHECK(destroyed.load() == 2);
}

TEST_CASE("Clearing an already-empty chain retires nothing", "[plugin][hosted][retirement]")
{
    // Every setStateInformation restore clears the chain. With no audio
    // running (pluginval's state tests, a host restoring presets while
    // stopped) the reaper only frees on quiescent ticks, so retiring an empty
    // snapshot per restore overflowed kMaxRetiredChains (a Debug jassert).
    juce::ScopedJuceInitialiser_GUI juceInit;
    std::atomic<int> destroyed{0};
    HostedPluginChainManager manager;
    manager.prepareToPlay(48000.0, 128, 2);
    for (int i = 0; i < 40; ++i) manager.clear();
    CHECK(manager.getNumRetiredChains() == 0);

    // A real chain is still retired (and released) on clear.
    manager.addPluginInstance(std::make_unique<DtorCountingInstance>(destroyed), "A");
    const auto before = manager.getNumRetiredChains();
    manager.clear();
    CHECK(manager.getNumRetiredChains() == before + 1);
    CHECK(manager.getSlotInfo().empty());
    for (int i = 0; i < 40; ++i) manager.clear();
    CHECK(manager.getNumRetiredChains() == before + 1);
}

TEST_CASE("A hosted sidechain bus is disabled, or the slot runs on a wide scratch", "[plugin][hosted][sidechain]")
{
    // TDR Nova comes up with a mono sidechain enabled (3 input channels);
    // handing it the host's 2-channel buffer crashed the plugin.
    for (const bool refuseDisable : {false, true}) {
        INFO("refuseDisable = " << refuseDisable);
        HostedPluginChainManager manager;
        manager.prepareToPlay(48000.0, 256, 2);

        auto instance = std::make_unique<SidechainInstance>(refuseDisable);
        auto* raw = instance.get();
        manager.addPluginInstance(std::move(instance), "Sidechain");
        CHECK(raw->getTotalNumInputChannels() == (refuseDisable ? 3 : 2));

        // Larger than the prepared block, so the split sub-blocks go through
        // the scratch path too.
        juce::AudioBuffer<float> buffer(2, 600);
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < 600; ++i) buffer.setSample(c, i, 0.8f);
        manager.processBlock(buffer, false, nullptr);

        CHECK(raw->undersizedBuffers.load() == 0);
        CHECK(raw->processed.load() == 3);
        for (int c = 0; c < 2; ++c) {
            CHECK(buffer.getSample(c, 0) == Catch::Approx(0.4f));
            CHECK(buffer.getSample(c, 599) == Catch::Approx(0.4f));
        }

        // Partial wet mixes the processed signal with the dry one (after the
        // per-sample wet ramp has settled - AUDIT-061).
        manager.setWetMix(0, 0.5f);
        for (int pass = 0; pass < 4; ++pass) {
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < 600; ++i) buffer.setSample(c, i, 0.8f);
            manager.processBlock(buffer, false, nullptr);
        }
        CHECK(raw->undersizedBuffers.load() == 0);
        CHECK(buffer.getSample(1, 300) == Catch::Approx(0.6f));
    }
}

TEST_CASE("A stereo hosted plugin in a mono session never sees a 1-channel table", "[plugin][hosted][sidechain]")
{
    HostedPluginChainManager manager;
    manager.prepareToPlay(48000.0, 256, 1);
    auto instance = std::make_unique<SidechainInstance>(true); // stereo-only, can't go mono
    auto* raw = instance.get();
    manager.addPluginInstance(std::move(instance), "StereoOnly");

    juce::AudioBuffer<float> buffer(1, 256);
    for (int i = 0; i < 256; ++i) buffer.setSample(0, i, 0.8f);
    manager.processBlock(buffer, false, nullptr);
    CHECK(raw->undersizedBuffers.load() == 0);
    CHECK(raw->processed.load() == 1);
    CHECK(buffer.getSample(0, 100) == Catch::Approx(0.4f));
    // AUDIT-028: the main-bus right input carries the mono signal (not
    // silence); only the sidechain input is silent.
    CHECK(raw->lastRightInput.load() == Catch::Approx(0.8f));
    CHECK(raw->lastSidechainInput.load() == Catch::Approx(0.0f));
}

TEST_CASE("Moving a slot's wet control never changes the reported latency", "[plugin][hosted][audit061][fix14]")
{
    HostedPluginChainManager manager;
    manager.prepareToPlay(48000.0, 256, 2);
    manager.addPluginInstance(std::make_unique<khensu::test::FakeLatencyInstance>(100), "Lat");

    // The old 1 % / 99 % thresholds made the latency jump mid-drag.
    for (const float wet : {1.0f, 0.995f, 0.5f, 0.011f, 0.009f, 0.0f}) {
        INFO("wet = " << wet);
        manager.setWetMix(0, wet);
        CHECK(manager.getActiveLatency() == 100);
    }
    // Nor does bypassing it (FIX-14): the slot then plays the dry signal
    // delayed by the same amount.
    manager.setBypass(0, true);
    CHECK(manager.getActiveLatency() == 100);
}

TEST_CASE("The dry compensation line is fed at full wet, so pulling wet down replays no stale audio",
          "[plugin][hosted][audit061]")
{
    HostedPluginChainManager manager;
    manager.prepareToPlay(48000.0, 256, 2);
    manager.addPluginInstance(std::make_unique<khensu::test::FakeLatencyInstance>(100), "Lat");

    // A constant 0.5 signal: the latency-compensated dry and the delayed wet
    // are both 0.5, so ANY deviation after the first `latency` samples is
    // stale (zero) dry history leaking in.
    juce::AudioBuffer<float> buffer(2, 256);
    float minAfterWarmup = 1.0f;
    for (int block = 0; block < 24; ++block) {
        // Fully wet until now: the old code skipped feeding the dry line
        // there, so going partial read 100 samples of never-written zeros.
        if (block == 8) manager.setWetMix(0, 0.5f);
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < 256; ++i) buffer.setSample(c, i, 0.5f);
        manager.processBlock(buffer, false, nullptr);
        if (block > 0) // block 0 carries the plugin's own 100-sample startup delay
            for (int c = 0; c < 2; ++c)
                minAfterWarmup = juce::jmin(minAfterWarmup,
                                            juce::FloatVectorOperations::findMinimum(buffer.getReadPointer(c), 256));
    }
    CHECK(minAfterWarmup == Catch::Approx(0.5f).margin(1.0e-6));
}

TEST_CASE("Wet changes ramp per sample instead of stepping per block", "[plugin][hosted][audit061]")
{
    HostedPluginChainManager manager;
    manager.prepareToPlay(48000.0, 256, 2);
    // Sidechain-free halving plugin: wet output 0.4 for a 0.8 input.
    manager.addPluginInstance(std::make_unique<SidechainInstance>(false), "Half");

    juce::AudioBuffer<float> buffer(2, 256);
    const auto fill = [&buffer] {
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < 256; ++i) buffer.setSample(c, i, 0.8f);
    };
    fill();
    manager.processBlock(buffer, false, nullptr);
    CHECK(buffer.getSample(0, 255) == Catch::Approx(0.4f));

    manager.setWetMix(0, 0.0f);
    fill();
    manager.processBlock(buffer, false, nullptr);
    float maxStep = 0.0f;
    for (int i = 1; i < 256; ++i) {
        CHECK(buffer.getSample(0, i) >= buffer.getSample(0, i - 1) - 1.0e-7f); // monotonic toward dry
        maxStep = juce::jmax(maxStep, buffer.getSample(0, i) - buffer.getSample(0, i - 1));
    }
    // Mid-ramp after one block (the 20 ms ramp is ~960 samples at 48 kHz).
    CHECK(buffer.getSample(0, 255) > 0.41f);
    CHECK(buffer.getSample(0, 255) < 0.79f);
    CHECK(maxStep < 0.01f);
}

TEST_CASE("A slot the audio path cannot run is not counted as latency", "[plugin][hosted][audit026]")
{
    HostedPluginChainManager manager;
    manager.prepareToPlay(48000.0, 256, 2);
    auto wide = std::make_unique<khensu::test::WideBusInstance>();
    auto* raw = wide.get();
    manager.addPluginInstance(std::move(wide), "Wide");
    REQUIRE(raw->getTotalNumInputChannels() == 32);

    juce::AudioBuffer<float> buffer(2, 256);
    buffer.clear();
    manager.processBlock(buffer, false, nullptr);
    CHECK(raw->processed.load() == 0);          // >= 32 channels: skipped (AUDIT-085)
    CHECK(manager.getActiveLatency() == 0);     // ...so its 100 samples are not reported
}

TEST_CASE("A hosted latency change after insert is reported through the callback", "[plugin][hosted][audit026]")
{
    HostedPluginChainManager manager;
    std::atomic<int> notified{0};
    manager.setLatencyChangedCallback([&notified] { notified.fetch_add(1); });
    manager.prepareToPlay(48000.0, 256, 2);
    auto fake = std::make_unique<khensu::test::FakeLatencyInstance>(64);
    auto* raw = fake.get();
    manager.addPluginInstance(std::move(fake), "Lat");
    CHECK(manager.getActiveLatency() == 64);

    raw->changeReportedLatency(300);
    CHECK(notified.load() == 1);
    CHECK(manager.getActiveLatency() == 300);

    // A removed slot no longer calls back.
    manager.removePlugin(0);
    manager.reap();
}

TEST_CASE("Hosted tail length is the longest active tail, with a finite fallback for infinite tails",
          "[plugin][hosted][audit062]")
{
    using khensu::test::FakeLatencyInstance;
    HostedPluginChainManager manager;
    manager.prepareToPlay(48000.0, 256, 2);
    CHECK(manager.getTailLengthSeconds() == Catch::Approx(0.0));

    manager.addPluginInstance(std::make_unique<FakeLatencyInstance>(0, 2.5), "Reverb");
    manager.addPluginInstance(std::make_unique<FakeLatencyInstance>(0, 4.0), "Delay");
    CHECK(manager.getTailLengthSeconds() == Catch::Approx(4.0));

    manager.setBypass(1, true); // a bypassed slot carries no tail
    CHECK(manager.getTailLengthSeconds() == Catch::Approx(2.5));

    manager.addPluginInstance(std::make_unique<FakeLatencyInstance>(0, std::numeric_limits<double>::infinity()),
                              "Freeze");
    CHECK(manager.getTailLengthSeconds() == Catch::Approx(HostedPluginChainManager::kInfiniteTailFallbackSeconds));
}

TEST_CASE("Non-finite wet amounts restore as fully wet and are ignored as control input", "[plugin][hosted][audit085]")
{
    HostedPluginChainManager manager;
    manager.prepareToPlay(48000.0, 256, 2);
    manager.addPluginInstance(std::make_unique<RecordingInstance>(), "A", {}, false,
                              std::numeric_limits<float>::quiet_NaN());
    CHECK(manager.getSlotInfo()[0].wetMix == Catch::Approx(1.0f));

    manager.setWetMix(0, 0.25f);
    manager.setWetMix(0, std::numeric_limits<float>::quiet_NaN());
    CHECK(manager.getSlotInfo()[0].wetMix == Catch::Approx(0.25f));

    HostedPluginChainManager::PersistedSlot saved;
    saved.filePath = "C:/plugins/x.vst3";
    saved.wetMix = std::numeric_limits<float>::infinity();
    manager.addPlaceholder(saved);
    CHECK(manager.getSlotInfo()[1].wetMix == Catch::Approx(1.0f));
}

TEST_CASE("Bypassing a slot keeps the reported latency and plays the dry signal delayed by it (FIX-14)",
          "[plugin][hosted][fix14]")
{
    using khensu::test::FakeLatencyInstance;
    HostedPluginChainManager manager;
    manager.prepareToPlay(48000.0, 256, 2);
    manager.addPluginInstance(std::make_unique<FakeLatencyInstance>(100), "Lat");
    REQUIRE(manager.getActiveLatency() == 100);

    // The host is told the latency once. A bypass that changed it made the
    // host re-plan its delay compensation, and moved the audio by 100 samples.
    manager.setBypass(0, true);
    CHECK(manager.getActiveLatency() == 100);

    juce::AudioBuffer<float> buffer(2, 256);
    int reported = 100;
    for (int block = 0; block < 8; ++block) { // let the bypass fade finish
        buffer.clear();
        reported = juce::jmin(reported, manager.processBlock(buffer, false, nullptr));
    }
    CHECK(reported == 100);

    buffer.clear();
    buffer.setSample(0, 0, 1.0f);
    buffer.setSample(1, 0, 1.0f);
    CHECK(manager.processBlock(buffer, false, nullptr) == 100);
    for (int c = 0; c < 2; ++c) {
        CHECK(buffer.getSample(c, 0) == Catch::Approx(0.0f).margin(1.0e-6));
        CHECK(buffer.getSample(c, 100) == Catch::Approx(1.0f).margin(1.0e-6));
    }
}

TEST_CASE("Bypassing a slot crossfades to the dry signal instead of switching (FIX-14)", "[plugin][hosted][fix14]")
{
    HostedPluginChainManager manager;
    manager.prepareToPlay(48000.0, 256, 2);
    // Sidechain-free halving plugin: wet output 0.4 for a 0.8 input.
    manager.addPluginInstance(std::make_unique<SidechainInstance>(false), "Half");

    juce::AudioBuffer<float> buffer(2, 256);
    const auto run = [&] {
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < 256; ++i) buffer.setSample(c, i, 0.8f);
        manager.processBlock(buffer, false, nullptr);
    };
    const auto maxStep = [&](float previous) {
        float step = 0.0f;
        for (int i = 0; i < 256; ++i) {
            step = juce::jmax(step, std::abs(buffer.getSample(0, i) - previous));
            previous = buffer.getSample(0, i);
        }
        return step;
    };
    run();
    REQUIRE(buffer.getSample(0, 255) == Catch::Approx(0.4f));

    manager.setBypass(0, true);
    run();
    CHECK(maxStep(0.4f) < 0.01f);            // no jump at the switch
    CHECK(buffer.getSample(0, 255) > 0.41f); // on its way to the dry signal
    CHECK(buffer.getSample(0, 255) < 0.79f);
    for (int block = 0; block < 8; ++block) run();
    CHECK(buffer.getSample(0, 255) == Catch::Approx(0.8f)); // bypassed: the dry signal

    manager.setBypass(0, false);
    run();
    CHECK(maxStep(0.8f) < 0.01f); // and back the same way
    for (int block = 0; block < 8; ++block) run();
    CHECK(buffer.getSample(0, 255) == Catch::Approx(0.4f));
}

TEST_CASE("A hosted latency above one second is compensated in full (FIX-14)", "[plugin][hosted][fix14]")
{
    using khensu::test::FakeLatencyInstance;
    constexpr int kLatency = 60000; // 1.25 s at 48 kHz
    constexpr int kImpulseBlock = 8; // after the wet ramp has settled
    HostedPluginChainManager manager;
    manager.prepareToPlay(48000.0, 256, 2);
    manager.addPluginInstance(std::make_unique<FakeLatencyInstance>(kLatency), "Long");
    manager.setWetMix(0, 0.5f);
    CHECK(manager.getActiveLatency() == kLatency);

    // Half wet, one impulse in: the dry half must arrive with the wet half.
    // With the dry line capped at one second it arrived 12000 samples early,
    // while the host was told the full 60000.
    juce::AudioBuffer<float> buffer(2, 256);
    int peaks = 0, peakAt = -1, reported = kLatency;
    float peakValue = 0.0f;
    const int blocks = kImpulseBlock + kLatency / 256 + 4;
    for (int block = 0; block < blocks; ++block) {
        buffer.clear();
        if (block == kImpulseBlock) {
            buffer.setSample(0, 0, 1.0f);
            buffer.setSample(1, 0, 1.0f);
        }
        reported = juce::jmin(reported, manager.processBlock(buffer, false, nullptr));
        for (int i = 0; i < 256; ++i) {
            if (std::abs(buffer.getSample(0, i)) <= 0.1f) continue;
            ++peaks;
            peakAt = (block - kImpulseBlock) * 256 + i;
            peakValue = buffer.getSample(0, i);
        }
    }
    CHECK(reported == kLatency);
    CHECK(peaks == 1);
    CHECK(peakAt == kLatency);
    CHECK(peakValue == Catch::Approx(1.0f).margin(0.01));
}

TEST_CASE("A latency raised past a slot's dry line after insert is compensated in full (FIX-14)",
          "[plugin][hosted][fix14]")
{
    using khensu::test::FakeLatencyInstance;
    constexpr int kLatency = 72000; // 1.5 s at 48 kHz; the slot's line was sized for 1 s
    HostedPluginChainManager manager;
    manager.prepareToPlay(48000.0, 256, 2);
    auto fake = std::make_unique<FakeLatencyInstance>(64);
    auto* raw = fake.get();
    manager.addPluginInstance(std::move(fake), "Grows");
    manager.setWetMix(0, 0.5f);

    raw->changeLatency(kLatency);
    CHECK(manager.getActiveLatency() == 48000); // clamped until the line grows
    manager.reserveDryLines();                  // what the processor does on the change
    CHECK(manager.getActiveLatency() == kLatency);

    // Half wet, one impulse in: the dry half arrives with the wet half.
    constexpr int kImpulseBlock = 8;
    juce::AudioBuffer<float> buffer(2, 256);
    int peaks = 0, peakAt = -1, processed = 0;
    const int blocks = kImpulseBlock + kLatency / 256 + 4;
    for (int block = 0; block < blocks; ++block) {
        buffer.clear();
        if (block == kImpulseBlock) {
            buffer.setSample(0, 0, 1.0f);
            buffer.setSample(1, 0, 1.0f);
        }
        processed = manager.processBlock(buffer, false, nullptr);
        for (int i = 0; i < 256; ++i) {
            if (std::abs(buffer.getSample(0, i)) <= 0.1f) continue;
            ++peaks;
            peakAt = (block - kImpulseBlock) * 256 + i;
        }
    }
    CHECK(processed == kLatency); // the audio thread adopted the bigger line
    CHECK(peaks == 1);
    CHECK(peakAt == kLatency);
    manager.reap(); // frees the replaced line
}

TEST_CASE("A mono-only hosted plugin on a stereo track processes both channels, aligned (FIX-14)",
          "[plugin][hosted][fix14]")
{
    using khensu::test::MonoOnlyInstance;
    HostedPluginChainManager manager;
    manager.prepareToPlay(48000.0, 256, 2);
    auto mono = std::make_unique<MonoOnlyInstance>(64);
    auto* raw = mono.get();
    manager.addPluginInstance(std::move(mono), "Mono");
    REQUIRE(raw->getTotalNumInputChannels() == 1);
    REQUIRE(raw->getTotalNumOutputChannels() == 1);
    CHECK(manager.getActiveLatency() == 64);

    // An impulse on the right only. Handed the stereo buffer, the plugin
    // processed the (silent) left and the right went out dry and undelayed.
    juce::AudioBuffer<float> buffer(2, 256);
    buffer.clear();
    buffer.setSample(1, 0, 1.0f);
    manager.processBlock(buffer, false, nullptr);
    CHECK(raw->maxChannelsSeen.load() == 1);
    for (int c = 0; c < 2; ++c) {
        INFO("channel " << c);
        CHECK(buffer.getSample(c, 0) == Catch::Approx(0.0f).margin(1.0e-6));
        // The mono sum (0 + 1) / 2, halved by the plugin, 64 samples later.
        CHECK(buffer.getSample(c, 64) == Catch::Approx(0.25f).margin(1.0e-6));
    }

    // Steady state: both channels carry the processed mono sum.
    for (int block = 0; block < 4; ++block) {
        for (int i = 0; i < 256; ++i) {
            buffer.setSample(0, i, 0.8f);
            buffer.setSample(1, i, 0.4f);
        }
        manager.processBlock(buffer, false, nullptr);
    }
    CHECK(buffer.getSample(0, 255) == Catch::Approx(0.3f).margin(1.0e-6));
    CHECK(buffer.getSample(1, 255) == Catch::Approx(0.3f).margin(1.0e-6));
}
