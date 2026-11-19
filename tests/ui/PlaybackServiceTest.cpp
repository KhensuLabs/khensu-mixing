// tests/ui/PlaybackServiceTest.cpp
// Pins PlaybackService (ENG-1 stage C, extracted from AppController): the A/B
// switch, loudness matching of B, the render-history entry point and the
// session reset - without an AppController. Background work runs inline;
// results that hop back to the message thread are pumped with the dispatch
// loop (KhensuTests builds with JUCE_MODAL_LOOPS_PERMITTED).
#include <catch2/catch_test_macros.hpp>
#include "IBackgroundWork.h"
#include "session/PlaybackService.h"
#include "session/SessionService.h"
#include "support/NullServices.h"
#include "support/ScopedTempDir.h"

using khensu::PlaybackService;
using khensu::SessionService;

namespace {
struct InlineWork final : khensu::IBackgroundWork {
    void runInBackground(std::function<void()> job) override { job(); }
    void runLongInBackground(std::function<void()> job) override { job(); }
    uint64_t sessionEpoch() const noexcept override { return 0; }
    bool isShuttingDown() const noexcept override { return false; }
};

struct Fixture {
    khensu::test::ScopedTempDir tmp{"khensu-playback-service-"}; // first: deleted after the services close khensu.db
    juce::File dir = tmp.file;
    InlineWork work;
    SessionService sessions;
    khensu::test::NullStems stems;
    khensu::test::NullAnalysisHub analysis;
    std::unique_ptr<PlaybackService> playback;
    juce::File original, render;

    Fixture()
    {
        dir.createDirectory();
        original = writeSine("original.wav", 0.25f);
        render = writeSine("render.wav", 0.5f);
        sessions.attachStore(
            SessionService::openStore(dir.getChildFile("khensu.db").getFullPathName().toStdString(), 1).manager);
        playback = std::make_unique<PlaybackService>(work, sessions, stems, analysis);
    }
    ~Fixture()
    {
        playback.reset();
    }

    juce::File writeSine(const juce::String& name, float amplitude) const
    {
        const auto file = dir.getChildFile(name);
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream = file.createOutputStream();
        REQUIRE(stream != nullptr);
        const auto options =
            juce::AudioFormatWriterOptions{}.withSampleRate(44100.0).withNumChannels(2).withBitsPerSample(16);
        auto writer = wav.createWriterFor(stream, options);
        REQUIRE(writer != nullptr);
        juce::AudioBuffer<float> buf(2, 44100);
        for (int i = 0; i < buf.getNumSamples(); ++i) {
            const float v = amplitude * std::sin(juce::MathConstants<float>::twoPi * 440.0f * (float)i / 44100.0f);
            buf.setSample(0, i, v);
            buf.setSample(1, i, v);
        }
        writer->writeFromAudioSampleBuffer(buf, 0, buf.getNumSamples());
        return file;
    }

    void openSession()
    {
        khensu::AudioFileRef ref;
        ref.id = "f";
        ref.path = original.getFullPathName().toStdString();
        ref.format = "wav";
        auto created = sessions.create("t", {ref}, khensu::CreativeTarget{});
        REQUIRE(created.has_value());
        sessions.adopt(*created);
        playback->loadAudioFileForPlayback(original);
    }

    template <typename Pred> static bool pumpUntil(Pred pred, int timeoutMs = 5000)
    {
        const auto start = juce::Time::getMillisecondCounter();
        while (!pred()) {
            if (juce::Time::getMillisecondCounter() - start > static_cast<juce::uint32>(timeoutMs)) return false;
            juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
        }
        return true;
    }
};
} // namespace

TEST_CASE("PlaybackService: the A/B switch needs an open session and a B render", "[split][playback]")
{
    Fixture f;
    f.playback->setAbSide(true);
    f.playback->toggleAb();
    CHECK_FALSE(f.playback->hasAbComparison());
    CHECK_FALSE(f.playback->isPlayingProcessed());

    f.openSession();
    CHECK(f.playback->currentAuditionPath() == f.original.getFullPathName());

    int applied = 0;
    f.playback->addAbRenderAppliedListener(&applied, [&] { ++applied; });
    f.playback->setAbProcessedPath(f.render.getFullPathName());
    CHECK(f.playback->hasAbComparison());
    CHECK(f.playback->abProcessedPath() == f.render.getFullPathName());
    CHECK(applied == 1);

    f.playback->setAbSide(true);
    CHECK(f.playback->isPlayingProcessed());
    CHECK(f.playback->currentAuditionPath() == f.render.getFullPathName());
    f.playback->toggleAb();
    CHECK_FALSE(f.playback->isPlayingProcessed());
    CHECK(f.playback->currentAuditionPath() == f.original.getFullPathName());
    f.playback->removeAbRenderAppliedListener(&applied);
}

TEST_CASE("PlaybackService measures B's loudness before it is matched", "[split][playback]")
{
    Fixture f;
    f.openSession();
    f.playback->setAbProcessedPath(f.render.getFullPathName());
    // The measurement runs inline; its result is applied on the message thread.
    REQUIRE(Fixture::pumpUntil([&] { return f.playback->abProcessedLufs().has_value(); }));
    CHECK(*f.playback->abProcessedLufs() < 0.0f);
    CHECK(f.playback->isAbLoudnessKnown(f.render.getFullPathName()));

    // A level from elsewhere (a render's own guardrail) is cached the same way.
    f.playback->rememberLufs(f.original.getFullPathName(), -14.0f);
    CHECK(f.playback->isAbLoudnessKnown(f.original.getFullPathName()));
}

TEST_CASE("PlaybackService follows the render history and forgets a session", "[split][playback]")
{
    Fixture f;
    f.openSession();

    f.playback->loadAcceptedRender(f.render.getFullPathName());
    CHECK(f.playback->isPlayingProcessed());
    CHECK(f.playback->abProcessedPath() == f.render.getFullPathName());
    CHECK(f.playback->currentAuditionPath() == f.render.getFullPathName());

    f.playback->loadAcceptedRender({}); // back to the original
    CHECK_FALSE(f.playback->isPlayingProcessed());
    CHECK(f.playback->currentAuditionPath() == f.original.getFullPathName());

    f.playback->resetForSession();
    CHECK_FALSE(f.playback->hasAbComparison());
    CHECK(f.playback->currentAuditionPath().isEmpty());
    CHECK(f.playback->getWaveformPeaks().empty());
}

TEST_CASE("PlaybackService fans the plugin meters out to listeners", "[split][playback]")
{
    Fixture f;
    float seenLufs = 0.0f;
    f.playback->addRealTimeMeterListener(&seenLufs,
                                         [&](float, float, float mom, float, float, float) { seenLufs = mom; });
    f.playback->publishMeters(-6.0f, -6.5f, -12.0f, -13.0f, 0.9f, 0.4f);
    CHECK(seenLufs == -12.0f);
    f.playback->removeRealTimeMeterListener(&seenLufs);
    f.playback->publishMeters(-6.0f, -6.5f, -20.0f, -13.0f, 0.9f, 0.4f);
    CHECK(seenLufs == -12.0f);
}
