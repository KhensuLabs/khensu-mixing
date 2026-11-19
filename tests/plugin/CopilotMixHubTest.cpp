// tests/plugin/CopilotMixHubTest.cpp
// The cross-instance Mix hub: band grid, role guessing, track-vs-track
// masking, the summed view, stale-instance expiry, real-time safety of the
// audio-thread publish path, and several live processors sharing one hub.
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "../../plugin-copilot/Source/PluginProcessor.h"
#include "../../plugin-copilot/Source/mixhub/MixHub.h"
#include "../../plugin-copilot/Source/mixhub/MixHubAnalysis.h"
#include "../../plugin-copilot/Source/ui/MixHubPanel.h"
#include "AllocationGuard.h"

#include <atomic>
#include <cmath>
#include <thread>

using namespace khensu;
using namespace khensu::mixhub;
using Catch::Approx;

namespace {
constexpr double kSr = 48000.0;
constexpr int kBlock = 512;

int bandFor(float hz)
{
    for (int b = 0; b < kNumBands; ++b)
        if (hz >= bandLowEdgeHz(b) && hz < bandHighEdgeHz(b)) return b;
    return -1;
}

/// A frame that is loud (levelDb) in the given bands and quiet elsewhere.
TrackFrame frameWith(juce::uint64 id, const juce::String& name, std::initializer_list<std::pair<float, float>> hzDb,
                     float lufs = -14.0f, double updatedMs = 1000.0)
{
    TrackFrame f;
    f.instanceId = id;
    f.trackName = name;
    f.bandDb.fill(-85.0f);
    for (const auto& [hz, db] : hzDb) f.bandDb[(size_t)bandFor(hz)] = db;
    f.role = guessRole(name, f.bandDb);
    f.shortTermLufs = lufs;
    f.peakDb = -3.0f;
    f.width = 0.3f;
    f.updatedMs = updatedMs;
    return f;
}

/// Writes `blocks` blocks of a sum of sines (Hz, amplitude) into `proc`.
/// `guarded` arms the allocation guard around each processBlock call only, so
/// the test's own buffer is not counted (the guard sees malloc since VER-5).
void play(KhensuCopilotProcessor& proc, std::initializer_list<std::pair<double, float>> partials, int blocks,
          double& phaseSamples, bool guarded = false)
{
    juce::AudioBuffer<float> buffer(2, kBlock);
    juce::MidiBuffer midi;
    for (int b = 0; b < blocks; ++b) {
        for (int i = 0; i < kBlock; ++i) {
            double v = 0.0;
            for (const auto& [hz, amp] : partials)
                v += amp * std::sin(2.0 * juce::MathConstants<double>::pi * hz * (phaseSamples + i) / kSr);
            buffer.setSample(0, i, (float)v);
            buffer.setSample(1, i, (float)v);
        }
        phaseSamples += kBlock;
        if (guarded) {
            khensu::test::ScopedAllocationGuard guard;
            proc.processBlock(buffer, midi);
        } else {
            proc.processBlock(buffer, midi);
        }
    }
}

void prepare(KhensuCopilotProcessor& proc, const juce::String& trackName)
{
    proc.setPlayConfigDetails(2, 2, kSr, kBlock);
    proc.prepareToPlay(kSr, kBlock);
    juce::AudioProcessor::TrackProperties props;
    props.name = trackName;
    proc.updateTrackProperties(props);
}
} // namespace

TEST_CASE("Mix hub band grid is contiguous, log-spaced and covers 25 Hz-16 kHz", "[plugin][mixhub]")
{
    CHECK(bandCentreHz(0) == Approx(25.0f));
    CHECK(bandCentreHz(kNumBands - 1) == Approx(16000.0f).epsilon(1e-4));
    for (int b = 1; b < kNumBands; ++b) {
        CHECK(bandLowEdgeHz(b) == Approx(bandHighEdgeHz(b - 1)).epsilon(1e-5));
        CHECK(bandCentreHz(b) / bandCentreHz(b - 1) == Approx(bandCentreHz(1) / bandCentreHz(0)).epsilon(1e-5));
    }
    CHECK(formatHz(59.7f) == "60 Hz");
    CHECK(formatHz(254.0f) == "250 Hz");
    CHECK(formatHz(2540.0f) == "2.5 kHz");
    CHECK(formatHz(12500.0f) == "13 kHz");
}

TEST_CASE("Band levels fold a power spectrum, including bands narrower than one bin", "[plugin][mixhub]")
{
    const double binHz = 23.4375; // 2048-point FFT at 48 kHz: coarser than the lowest bands
    std::vector<float> power(1024, 0.0f);
    power[(size_t)std::lround(1000.0 / binHz)] = 0.5f; // a full-scale 1 kHz sine
    power[2] = 0.01f;                                  // ~47 Hz
    const auto bands = bandLevelsFromPower(power.data(), (int)power.size(), binHz);
    CHECK(bands[(size_t)bandFor(1000.0f)] == Approx(-3.01f).margin(0.05f));
    CHECK(bands[(size_t)bandFor(47.0f)] > -40.0f); // the narrow band still sees its bin
    CHECK(bands[(size_t)bandFor(5000.0f)] == kSilentDb);
}

TEST_CASE("Roles come from the host track name, then the spectrum; buses are recognised", "[plugin][mixhub]")
{
    const auto bands = silentBands();
    CHECK(guessRole("Kick In", bands) == "Kick");
    CHECK(guessRole("808 sub", bands) == "Bass");
    CHECK(guessRole("Lead Vox Dbl", bands) == "Vocal");
    CHECK(guessRole("Drum Bus", bands) == "Drums");
    CHECK(guessRole("Master", bands) == kMixBusRole);
    CHECK(guessRole("Mix Bus", bands) == kMixBusRole);
    CHECK(guessRole("Audio 7", bands) == "Silent");
    CHECK(guessRole("Audio 7", frameWith(1, "", {{60.0f, -10.0f}}).bandDb) == "Low-end");
    CHECK(guessRole("Audio 8", frameWith(1, "", {{6000.0f, -10.0f}}).bandDb) == "Bright");
}

TEST_CASE("Masking finds the kick-vs-bass overlap and names it", "[plugin][mixhub]")
{
    const auto kick = frameWith(1, "Kick", {{60.0f, -12.0f}, {3000.0f, -30.0f}});
    const auto bass = frameWith(2, "Bass", {{60.0f, -14.0f}, {120.0f, -16.0f}});
    const auto hat = frameWith(3, "Hats", {{9000.0f, -20.0f}});
    const auto conflicts = findMaskingConflicts({kick, bass, hat});
    REQUIRE(conflicts.size() == 1);
    const auto& c = conflicts.front();
    CHECK(c.band == bandFor(60.0f));
    CHECK(c.involves(1));
    CHECK(c.involves(2));
    CHECK_FALSE(c.involves(3));
    CHECK(c.describe().startsWith("Kick vs Bass at " + formatHz(bandCentreHz(bandFor(60.0f)))));
}

TEST_CASE("Masking ignores level gaps, quiet bands, silent tracks and the mix bus", "[plugin][mixhub]")
{
    const auto kick = frameWith(1, "Kick", {{60.0f, -12.0f}});
    CHECK(findMaskingConflicts({kick, frameWith(2, "Bass", {{60.0f, -20.0f}})}).empty()); // 8 dB apart
    CHECK(findMaskingConflicts({frameWith(1, "Kick", {{60.0f, -65.0f}}), frameWith(2, "Bass", {{60.0f, -64.0f}})})
              .empty()); // below the floor
    // A band 30 dB under the bass's own loudest band doesn't matter to the bass.
    CHECK(findMaskingConflicts(
              {frameWith(1, "Pad", {{60.0f, -42.0f}}), frameWith(2, "Bass", {{60.0f, -40.0f}, {120.0f, -10.0f}})})
              .size() == 0);
    CHECK(findMaskingConflicts({kick, frameWith(2, "Silent", {})}).empty());
    CHECK(findMaskingConflicts({kick, frameWith(2, "Master", {{60.0f, -12.0f}})}).empty());
}

TEST_CASE("Masking conflicts are sorted strongest first and capped", "[plugin][mixhub]")
{
    std::vector<TrackFrame> frames{frameWith(1, "Kick", {{60.0f, -10.0f}}), frameWith(2, "Bass", {{60.0f, -10.5f}}),
                                   frameWith(3, "Vox", {{2000.0f, -30.0f}}), frameWith(4, "Gtr", {{2000.0f, -33.0f}})};
    const auto conflicts = findMaskingConflicts(frames);
    REQUIRE(conflicts.size() == 2);
    CHECK(conflicts[0].nameA == "Kick");
    CHECK(conflicts[0].score > conflicts[1].score);
    CHECK(findMaskingConflicts(frames, {}, 1).size() == 1);
}

TEST_CASE("The summed view power-sums tracks and skips the mix bus", "[plugin][mixhub]")
{
    const auto a = frameWith(1, "Kick", {{60.0f, -12.0f}}, -14.0f);
    const auto b = frameWith(2, "Vox", {{2000.0f, -12.0f}}, -14.0f);
    const auto bus = frameWith(3, "Master", {{60.0f, -6.0f}}, -8.0f);
    const auto summary = summarise({a, b, bus});
    CHECK(summary.contributingTracks == 2);
    CHECK(summary.estimatedLufs == Approx(-14.0f + 3.0103f).margin(0.01f));
    REQUIRE(summary.shares.size() == 2);
    CHECK(summary.shares[0].loudnessShare == Approx(0.5f).margin(1e-4));
    CHECK(summary.lowShare == Approx(0.5f).margin(0.01f));
    CHECK(summary.midShare == Approx(0.5f).margin(0.01f));
    CHECK(summary.mixBandDb[(size_t)bandFor(60.0f)] == Approx(-12.0f).margin(0.01f));
}

TEST_CASE("The hub expires frames that stopped updating", "[plugin][mixhub]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    MixHub hub;
    hub.publish(frameWith(1, "Kick", {{60.0f, -12.0f}}, -14.0f, 1000.0));
    hub.publish(frameWith(2, "Bass", {{60.0f, -12.0f}}, -14.0f, 3500.0));
    CHECK(hub.snapshot(2000.0).size() == 2);
    const auto later = hub.snapshot(1000.0 + MixHub::kDefaultExpiryMs + 1.0);
    REQUIRE(later.size() == 1);
    CHECK(later[0].instanceId == 2);
    hub.setExpiryMs(10000.0);
    CHECK(hub.snapshot(5000.0).size() == 2);
    // Re-publishing replaces, it does not duplicate.
    hub.publish(frameWith(1, "Kick 2", {{60.0f, -12.0f}}, -14.0f, 5000.0));
    const auto snap = hub.snapshot(5000.0);
    REQUIRE(snap.size() == 2);
    CHECK(snap[0].trackName == "Kick 2");
}

TEST_CASE("Three live processors share one hub: names, masking and the Mix tab", "[plugin][mixhub][multi-instance]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    KhensuCopilotProcessor kick, bass, hat;
    auto& hub = kick.getMixHub();
    hub.setAutoPublish(false); // the test drives publishAll with its own clock
    REQUIRE(&bass.getMixHub() == &hub);
    REQUIRE(&hat.getMixHub() == &hub);
    CHECK(hub.getNumRegistered() >= 3);
    CHECK(kick.getMixHubInstanceId() != bass.getMixHubInstanceId());

    prepare(kick, "Kick");
    prepare(bass, "Bass");
    prepare(hat, "Hats");
    CHECK(kick.getHostTrackName() == "Kick");

    double pk = 0, pb = 0, ph = 0;
    for (int tick = 1; tick <= 3; ++tick) {
        play(kick, {{60.0, 0.4f}}, 24, pk);
        play(bass, {{60.0, 0.35f}, {120.0, 0.15f}}, 24, pb);
        play(hat, {{9000.0, 0.1f}}, 24, ph);
        hub.publishAll(1000.0 * tick);
    }
    const auto frames = hub.snapshot(3000.0);
    REQUIRE(frames.size() == 3);
    for (const auto& f : frames) CHECK(f.hasSignal());

    // A 0.4-amplitude sine is -10.97 dB mean-square; the 60 Hz band must see it.
    const auto& kickFrame = *std::find_if(frames.begin(), frames.end(),
                                          [&](const TrackFrame& f) { return f.instanceId == kick.getMixHubInstanceId(); });
    CHECK(kickFrame.role == "Kick");
    CHECK(kickFrame.bandDb[(size_t)bandFor(60.0f)] == Approx(-10.97f).margin(1.0f));
    CHECK(kickFrame.bandDb[(size_t)bandFor(9000.0f)] < -60.0f);

    const auto conflicts = findMaskingConflicts(frames);
    REQUIRE_FALSE(conflicts.empty());
    CHECK(conflicts.front().describe().startsWith("Kick vs Bass at"));
    for (const auto& c : conflicts) CHECK_FALSE(c.involves(hat.getMixHubInstanceId()));

    MixHubPanel panel(kick);
    panel.setSize(600, 400);
    panel.refresh(3000.0);
    CHECK(panel.getNumInstanceRows() == 3);
    REQUIRE_FALSE(panel.getConflictLines().empty());
    CHECK(panel.getConflictLines().front().startsWith("Kick vs Bass"));
    CHECK(panel.getSummaryText().contains("3 instances"));
    CHECK(MixHubPanel::scopeNote().contains("separate processes"));
    juce::Image image(juce::Image::ARGB, 600, 400, true);
    juce::Graphics g(image);
    panel.paint(g); // draws without asserting

    // The hat track stops processing (bypassed/frozen): it ages out.
    for (int tick = 4; tick <= 8; ++tick) {
        play(kick, {{60.0, 0.4f}}, 12, pk);
        play(bass, {{60.0, 0.35f}}, 12, pb);
        hub.publishAll(1000.0 * tick);
    }
    const auto later = hub.snapshot(8000.0);
    CHECK(later.size() == 2);
    for (const auto& f : later) CHECK(f.instanceId != hat.getMixHubInstanceId());
}

TEST_CASE("A destroyed instance leaves the hub immediately", "[plugin][mixhub][multi-instance]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    KhensuCopilotProcessor keeper;
    auto& hub = keeper.getMixHub();
    hub.setAutoPublish(false);
    const int before = hub.getNumRegistered();
    juce::uint64 goneId = 0;
    {
        KhensuCopilotProcessor transient;
        prepare(transient, "Temp");
        double phase = 0;
        play(transient, {{440.0, 0.3f}}, 20, phase);
        hub.publishAll(1000.0);
        goneId = transient.getMixHubInstanceId();
        CHECK(hub.getNumRegistered() == before + 1);
        CHECK(hub.snapshot(1000.0).size() >= 1);
    }
    CHECK(hub.getNumRegistered() == before);
    for (const auto& f : hub.snapshot(1000.0)) CHECK(f.instanceId != goneId);
}

TEST_CASE("Publishing to the hub never allocates on the audio thread, even while the hub analyses",
          "[plugin][mixhub][cq03]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    KhensuCopilotProcessor proc;
    prepare(proc, "Kick");
    auto& hub = proc.getMixHub();
    hub.setAutoPublish(false);

    // Warm up: first-block lazy state is allowed to allocate.
    double phase = 0;
    play(proc, {{60.0, 0.3f}}, 8, phase);

    std::atomic<bool> stop{false};
    std::thread publisher([&] {
        double t = 0;
        while (!stop) {
            hub.publishAll(t += 50.0);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    khensu::test::resetViolations();
    play(proc, {{60.0, 0.3f}}, 400, phase, true);
    stop = true;
    publisher.join();
    CHECK(khensu::test::violationCount() == 0);
    khensu::test::resetViolations();
}

TEST_CASE("Instances can come and go while the publisher thread runs", "[plugin][mixhub][multi-instance]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    KhensuCopilotProcessor keeper;
    keeper.getMixHub().setAutoPublish(true);
    prepare(keeper, "Keeper");
    double phase = 0;
    for (int round = 0; round < 12; ++round) {
        KhensuCopilotProcessor transient;
        prepare(transient, "T" + juce::String(round));
        double p = 0;
        play(transient, {{200.0, 0.2f}}, 30, p);
        play(keeper, {{80.0, 0.2f}}, 10, phase);
        juce::Thread::sleep(15); // let the publisher overlap construction/destruction
    }
    CHECK(keeper.getMixHub().getNumRegistered() >= 1);
}
