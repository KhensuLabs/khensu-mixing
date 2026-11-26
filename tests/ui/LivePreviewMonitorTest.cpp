// tests/ui/LivePreviewMonitorTest.cpp
// STU-5: the transport's monitoring state (PlaybackService -> the engine's
// shared MonitorSolo) and how the bar words it. STU-9: the live preview stage
// (LivePreviewStage) swaps chains in, crossfades, loudness-matches and goes
// back to the plain signal; PlaybackService reports what a preview can run
// live, keeps one owner, and drops the preview on a session change; the EQ
// editor's chain is the one Apply EQ renders. (The DSP itself and its
// no-allocation checks: tests/shared-core/test_realtime_preview.cpp and
// tests/plugin/RealtimePreviewAllocationTest.cpp.)
#include <catch2/catch_test_macros.hpp>
#include "IBackgroundWork.h"
#include "services/EqBandChain.h"
#include "session/LivePreviewStage.h"
#include "session/PlaybackService.h"
#include "session/RenderService.h"
#include "session/SessionService.h"
#include "support/NullServices.h"
#include "support/ScopedTempDir.h"
#include "ui/shared/MonitorStateText.h"

#include <cmath>

using khensu::IPlayback;
using khensu::LivePreviewStage;
using khensu::PlaybackService;
using khensu::SessionService;

namespace {

struct InlineWork final : khensu::IBackgroundWork {
    void runInBackground(std::function<void()> job) override { job(); }
    void runLongInBackground(std::function<void()> job) override { job(); }
    uint64_t sessionEpoch() const noexcept override { return 0; }
    bool isShuttingDown() const noexcept override { return false; }
};

struct ServiceFixture {
    khensu::test::ScopedTempDir tmp{"khensu-live-preview-"};
    InlineWork work;
    SessionService sessions;
    khensu::test::NullStems stems;
    khensu::test::NullAnalysisHub analysis;
    std::unique_ptr<PlaybackService> playback;

    ServiceFixture()
    {
        tmp.file.createDirectory();
        sessions.attachStore(
            SessionService::openStore(tmp.file.getChildFile("khensu.db").getFullPathName().toStdString(), 1).manager);
        playback = std::make_unique<PlaybackService>(work, sessions, stems, analysis);
    }
    ~ServiceFixture() { playback.reset(); }
};

/// Noise in, with the block it produced kept for comparison.
struct NoiseSource final : juce::AudioSource {
    juce::Random rng{5};
    juce::AudioBuffer<float> last{2, 4096};
    void prepareToPlay(int, double) override {}
    void releaseResources() override {}
    void getNextAudioBlock(const juce::AudioSourceChannelInfo& info) override
    {
        for (int c = 0; c < info.buffer->getNumChannels(); ++c)
            for (int i = 0; i < info.numSamples; ++i) {
                const float v = rng.nextFloat() * 0.4f - 0.2f;
                info.buffer->setSample(c, info.startSample + i, v);
                last.setSample(c, i, v);
            }
    }
};

double rms(const juce::AudioBuffer<float>& b, int n)
{
    double s = 0.0;
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < n; ++i) s += (double)b.getSample(c, i) * b.getSample(c, i);
    return std::sqrt(s / (2.0 * n));
}

khensu::ipc::HttpServiceBridge::EffectSpec effect(const char* name, std::map<juce::String, float> params = {})
{
    khensu::ipc::HttpServiceBridge::EffectSpec e;
    e.effect = name;
    e.parameters = std::move(params);
    return e;
}

} // namespace

// ---- STU-5 ------------------------------------------------------------------

TEST_CASE("Transport monitoring state round-trips through the engine (STU-5)", "[stu5][playback]")
{
    ServiceFixture f;
    auto& pb = *f.playback;
    CHECK(pb.monitorMode() == IPlayback::MonitorMode::Full);
    CHECK_FALSE(pb.monitorDim());
    for (const auto mode : {IPlayback::MonitorMode::Mono, IPlayback::MonitorMode::Side, IPlayback::MonitorMode::Sub,
                            IPlayback::MonitorMode::Full}) {
        pb.setMonitorMode(mode);
        CHECK(pb.monitorMode() == mode);
    }
    pb.setMonitorDim(true);
    CHECK(pb.monitorDim());
    pb.setMonitorDim(false);
    CHECK_FALSE(pb.monitorDim());

    using Solo = khensu::dsp::MonitorSolo::Mode;
    CHECK(PlaybackService::toSoloMode(IPlayback::MonitorMode::Full) == Solo::Off);
    CHECK(PlaybackService::toSoloMode(IPlayback::MonitorMode::Mono) == Solo::Mono);
    CHECK(PlaybackService::toSoloMode(IPlayback::MonitorMode::Side) == Solo::Side);
    CHECK(PlaybackService::toSoloMode(IPlayback::MonitorMode::Sub) == Solo::Sub);
}

TEST_CASE("Monitoring badge says when the output is not the mix (STU-5)", "[stu5][playback]")
{
    namespace mt = khensu::monitor_text;
    using M = IPlayback::MonitorMode;
    CHECK(mt::badgeText(M::Full, false).isEmpty());
    CHECK(mt::buttonText(M::Full, false) == "Monitor");
    CHECK(mt::badgeText(M::Mono, false) == "MONITORING: MONO");
    CHECK(mt::badgeText(M::Side, true) == "MONITORING: SIDE + DIM");
    CHECK(mt::badgeText(M::Full, true) == "MONITORING: DIM");
    CHECK(mt::buttonText(M::Sub, true) == "Sub + Dim");
}

// ---- STU-9 ------------------------------------------------------------------

TEST_CASE("Live preview stage swaps chains in, matched, and back out (STU-9)", "[stu9][playback]")
{
    NoiseSource noise;
    LivePreviewStage stage(noise);
    constexpr int kBlock = 480;
    constexpr double kRate = 48000.0;
    stage.prepareToPlay(kBlock, kRate);
    juce::AudioBuffer<float> out(2, kBlock);
    const auto run = [&](int blocks) {
        for (int b = 0; b < blocks; ++b) stage.getNextAudioBlock(juce::AudioSourceChannelInfo(&out, 0, kBlock));
    };
    const auto equalsInput = [&] {
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < kBlock; ++i)
                if (!juce::exactlyEqual(out.getSample(c, i), noise.last.getSample(c, i))) return false;
        return true;
    };

    run(4);
    CHECK(equalsInput()); // nothing previewed: the plain signal

    // +12 dB and a big bell: the preview changes the sound, not the level.
    const auto& plan = stage.setChain({{"Gain", {{"gain_db", 12.0f}}},
                                       {"PeakFilter", {{"cutoff_frequency_hz", 2000.0f}, {"gain_db", 9.0f}}},
                                       {"MultibandCompressor", {}}});
    REQUIRE(plan.size() == 3);
    CHECK(plan[2].mode == khensu::dsp::RealtimeChain::StageMode::Skipped);
    CHECK(stage.isEnabled());
    run(static_cast<int>(4.0 * kRate / kBlock));
    CHECK_FALSE(equalsInput());
    double inSum = 0.0, outSum = 0.0;
    for (int b = 0; b < 50; ++b) {
        stage.getNextAudioBlock(juce::AudioSourceChannelInfo(&out, 0, kBlock));
        inSum += rms(noise.last, kBlock);
        outSum += rms(out, kBlock);
    }
    CHECK(std::abs(20.0 * std::log10(outSum / inSum)) < 1.0);

    // Many quick edits (a drag): every chain is handed over and collected.
    for (int k = 0; k < 40; ++k) {
        stage.setChain({{"PeakFilter", {{"cutoff_frequency_hz", 500.0f + 50.0f * (float)k}, {"gain_db", 4.0f}}}});
        run(1);
    }
    stage.collectGarbage();

    // Off: crossfades back to the plain signal, bit for bit.
    stage.clear();
    CHECK_FALSE(stage.isEnabled());
    run(10);
    CHECK(equalsInput());
}

TEST_CASE("PlaybackService reports and scopes the live preview (STU-9)", "[stu9][playback]")
{
    ServiceFixture f;
    auto& pb = *f.playback;
    CHECK_FALSE(pb.livePreviewStatus().active);

    int owner = 0, other = 0;
    pb.setLivePreviewChain(&owner,
                           {effect("PeakFilter", {{"cutoff_frequency_hz", 1000.0f}, {"gain_db", 3.0f}}),
                            effect("Limiter", {{"threshold_db", -1.0f}}), effect("DeEsser"), effect("Reverb")},
                           "EQ curve");
    auto status = pb.livePreviewStatus();
    CHECK(status.active);
    CHECK(status.label == "EQ curve");
    CHECK(status.live == juce::StringArray{"PeakFilter"});
    // STU-18: the de-esser has a causal stand-in too; Reverb needs the engine.
    REQUIRE(status.substituted.size() == 2);
    CHECK(status.substituted[0].startsWith("Limiter: "));
    CHECK(status.substituted[1].startsWith("DeEsser: "));
    REQUIRE(status.skipped.size() == 1);
    CHECK(status.skipped[0].startsWith("Reverb: "));
    CHECK(pb.isLivePreviewOwner(&owner));
    CHECK_FALSE(pb.isLivePreviewOwner(&other));

    pb.clearLivePreview(&other); // not theirs to stop
    CHECK(pb.livePreviewStatus().active);
    pb.clearLivePreview(&owner);
    CHECK_FALSE(pb.livePreviewStatus().active);
    CHECK_FALSE(pb.isLivePreviewOwner(&owner));

    // A session change drops an edit of the old track.
    pb.setLivePreviewChain(&owner, {effect("Gain", {{"gain_db", -3.0f}})}, "EQ curve");
    pb.resetForSession();
    CHECK_FALSE(pb.livePreviewStatus().active);
    CHECK_FALSE(pb.isLivePreviewOwner(&owner));
}

TEST_CASE("The EQ editor previews the chain Apply EQ renders (STU-9)", "[stu9][playback]")
{
    std::vector<khensu::EqBand> bands(4);
    bands[0].hz = 250.0f;
    bands[0].gain_db = -3.0f; // a cut: kept
    bands[1].gain_db = 0.05f; // inaudible: left out
    bands[2].type = khensu::EqBandType::HighPass;
    bands[2].hz = 35.0f; // no gain, still rendered
    bands[3].gain_db = 4.0f;
    bands[3].enabled = false; // off
    const auto chain = khensu::eqBandsToChain(bands);
    REQUIRE(chain.size() == 2);
    CHECK(chain[0].effect == "PeakFilter");
    CHECK(juce::exactlyEqual(chain[0].parameters.at("gain_db"), -3.0f));
    CHECK(chain[1].effect == "HighpassFilter");
    // Every stage the EQ editor can emit runs live, exactly as rendered.
    const auto status = PlaybackService::describePreview(
        [&] {
            khensu::dsp::RealtimeChain live;
            std::string error;
            REQUIRE(live.prepare(khensu::RenderService::nativeChain(chain), 48000.0, 2, 512, error));
            return live.plan();
        }(),
        "EQ curve");
    CHECK(status.live.size() == 2);
    CHECK(status.skipped.isEmpty());
    CHECK(status.substituted.isEmpty());
}
