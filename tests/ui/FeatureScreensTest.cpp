// tests/ui/FeatureScreensTest.cpp
// Regression tests for the 2026-11-06 audit's desktop feature-screen slice
// (UIX-*): render scope (stem renders never become the mix), export planning
// (as-mastered default, format extension, overwrite guards, post-gain
// compliance, source rate), chat error text and positional chain diffs,
// platform playback gain, take-alignment output paths and the 3D cloud's
// camera-space depth.
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <JuceHeader.h>
#include "ui/shared/SessionAudioPaths.h"
#include "ui/ExportDelivery/ExportPlan.h"
#include "ui/Copilot/ChatText.h"
#include "ui/VocalWorkspace/TakeAlignmentModel.h"
#include <khensu/targets/PlatformTargets.h>

using namespace khensu;

namespace {

/// The slice of the controller the session_audio helpers touch.
struct FakeRenderController {
    juce::File stemsDir;
    juce::String source;
    juce::String accepted;
    juce::String registered;
    juce::String loaded;

    juce::File getStemFile(const juce::String& name) const
    {
        return stemsDir == juce::File() ? juce::File() : stemsDir.getChildFile(name + ".wav");
    }
    juce::String activeSourcePath() const { return source; }
    juce::String currentRenderPath() const { return accepted; }
    // WF-4: the current vocal.
    juce::String editedVocal;
    juce::String currentVocalStem() const { return editedVocal; }
    FakeRenderController& stems() { return *this; }
    void setCurrentVocalStem(const juce::String& path) { editedVocal = path; }
    juce::String registeredInput;
    void registerAppliedRender(const juce::String& path, const juce::String&) { registered = path; }
    // WF-1: a Mix render that names its input registers through renders().
    void registerRenderOf(const juce::String& path, const juce::String& input, const juce::String&)
    {
        registered = path;
        registeredInput = input;
    }
    // applyRender reaches playback and renders through the hub accessors, like IAppController.
    FakeRenderController& playback() { return *this; }
    FakeRenderController& renders() { return *this; }
    void loadAudioFileForPlayback(const juce::File& f) { loaded = f.getFullPathName(); }
};

struct TempDir {
    juce::File dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                         .getChildFile("khensu_uix_" + juce::String(juce::Random::getSystemRandom().nextInt64()));
    TempDir() { dir.createDirectory(); }
    ~TempDir() { dir.deleteRecursively(); }
};

} // namespace

TEST_CASE("UIX-01: a stem or take render is auditioned, never registered as the mix", "[uix][render-scope]")
{
    TempDir tmp;
    FakeRenderController c;
    c.source = tmp.dir.getChildFile("song.wav").getFullPathName();
    c.stemsDir = tmp.dir.getChildFile("stems_1");
    c.stemsDir.createDirectory();
    const juce::String vocals = c.stemsDir.getChildFile("vocals.wav").getFullPathName();

    SECTION("input scope: only the session mix (or the accepted mix render) is Mix")
    {
        CHECK(session_audio::scopeForInput(c, c.source) == session_audio::RenderScope::Mix);
        CHECK(session_audio::scopeForInput(c, vocals) == session_audio::RenderScope::Stem);
        CHECK(session_audio::scopeForInput(c, "D:/takes/take2.wav") == session_audio::RenderScope::Stem);
        CHECK(session_audio::scopeForInput(c, {}) == session_audio::RenderScope::Stem);
        c.accepted = tmp.dir.getChildFile("song_master.wav").getFullPathName();
        CHECK(session_audio::scopeForInput(c, c.accepted) == session_audio::RenderScope::Mix);
    }

    SECTION("a Stem render loads for preview and leaves the render history alone")
    {
        const juce::String rendered = c.stemsDir.getChildFile("vocals_gainride_1.wav").getFullPathName();
        CHECK_FALSE(session_audio::applyRender(c, rendered, "s1", session_audio::RenderScope::Stem));
        CHECK(c.registered.isEmpty());
        CHECK(c.loaded == rendered);
    }

    SECTION("a vocal edit of the current vocal stacks: it becomes the current vocal (WF-4)")
    {
        REQUIRE(juce::File(vocals).replaceWithText("x"));
        const juce::String first = c.stemsDir.getChildFile("vocals_gainride_1.wav").getFullPathName();
        REQUIRE(juce::File(first).replaceWithText("x"));
        session_audio::applyRender(c, first, "s1", session_audio::RenderScope::Stem, vocals);
        CHECK(c.editedVocal == first);
        CHECK(session_audio::vocalStemOrSourcePath(c) == first);

        // The next tool processes the edit, and its render stacks on it.
        const juce::String second = c.stemsDir.getChildFile("vocals_gainride_1_tune_1.wav").getFullPathName();
        REQUIRE(juce::File(second).replaceWithText("x"));
        session_audio::applyRender(c, second, "s1", session_audio::RenderScope::Stem, first);
        CHECK(c.editedVocal == second);

        // A render of something else (a take, the raw stem after an edit) does not.
        const juce::String take = tmp.dir.getChildFile("take2_aligned.wav").getFullPathName();
        REQUIRE(juce::File(take).replaceWithText("x"));
        session_audio::applyRender(c, take, "s1", session_audio::RenderScope::Stem, "D:/takes/take2.wav");
        session_audio::applyRender(c, take, "s1", session_audio::RenderScope::Stem, vocals);
        CHECK(c.editedVocal == second);
    }

    SECTION("even a render tagged Mix is refused if it lives in the stems directory")
    {
        const juce::String rendered = c.stemsDir.getChildFile("vocals_x.wav").getFullPathName();
        CHECK_FALSE(session_audio::applyRender(c, rendered, "s1", session_audio::RenderScope::Mix));
        CHECK(c.registered.isEmpty());
    }

    SECTION("a Mix render registers")
    {
        const juce::String rendered = tmp.dir.getChildFile("song_gainride_1.wav").getFullPathName();
        CHECK(session_audio::applyRender(c, rendered, "s1", session_audio::RenderScope::Mix));
        CHECK(c.registered == rendered);
        CHECK(c.loaded.isEmpty());
    }
}

TEST_CASE("UIX-06 / 11.3: shared session path helpers", "[uix][render-scope]")
{
    TempDir tmp;
    FakeRenderController c;
    c.source = tmp.dir.getChildFile("song.wav").getFullPathName();
    c.stemsDir = tmp.dir.getChildFile("stems");
    c.stemsDir.createDirectory();

    // exportSourcePath: the accepted render only while it exists on disk.
    c.accepted = tmp.dir.getChildFile("song_master.wav").getFullPathName();
    CHECK(session_audio::exportSourcePath(c) == c.source);
    REQUIRE(juce::File(c.accepted).replaceWithText("x"));
    CHECK(session_audio::exportSourcePath(c) == c.accepted);

    // vocalStemOrSourcePath / activeStemPaths follow what exists.
    CHECK(session_audio::vocalStemOrSourcePath(c) == c.source);
    CHECK(session_audio::activeStemPaths(c).empty());
    REQUIRE(c.stemsDir.getChildFile("vocals.wav").replaceWithText("x"));
    CHECK(session_audio::vocalStemOrSourcePath(c) == c.stemsDir.getChildFile("vocals.wav").getFullPathName());
    const auto stems = session_audio::activeStemPaths(c);
    CHECK(stems.size() == 1);
    CHECK(stems.count("vocals") == 1);
}

TEST_CASE("UIX-03 / UIX-04: export path follows the format and never hits the session's own audio",
          "[uix][export]")
{
    using Fmt = AudioExportOptions::Format;
    CHECK(export_plan::withFormatExtension("C:/out/export.wav", Fmt::FLAC).endsWith("export.flac"));
    CHECK(export_plan::withFormatExtension("C:/out/export.wav", Fmt::MP3).endsWith("export.mp3"));
    CHECK(export_plan::withFormatExtension("C:/out/export.flac", Fmt::AAC).endsWith("export.m4a"));
    CHECK(export_plan::withFormatExtension({}, Fmt::WAV).isEmpty());

    // createLegalFileName: "Mix: v2?" must give a name Windows accepts.
    const auto name = export_plan::defaultFileName("Mix: v2?", Fmt::WAV);
    CHECK(name.endsWith(".wav"));
    CHECK_FALSE(name.containsAnyOf(":?*\"<>|/\\"));
    CHECK(export_plan::defaultFileName("   ", Fmt::FLAC) == "export.flac");

    const juce::String src = "C:/s/song.wav", render = "C:/s/song_master.wav", old = "C:/s/song_eq_1.wav";
    CHECK(export_plan::rejectOutputPath({}, src, render, {}).isNotEmpty());
    CHECK(export_plan::rejectOutputPath(src, src, render, {}).isNotEmpty());
    CHECK(export_plan::rejectOutputPath(render, src, render, {}).isNotEmpty());
    CHECK(export_plan::rejectOutputPath(old, src, render, {old}).isNotEmpty());
    CHECK(export_plan::rejectOutputPath("C:/out/delivery.wav", src, render, {old}).isEmpty());
}

TEST_CASE("UIX-02 / UIX-05: as-mastered applies no gain; compliance is graded after the export gain",
          "[uix][export]")
{
    // As mastered: unity, whatever the target.
    CHECK(export_plan::plannedGainDb(false, -9.0f, -14.0f, -0.5f, -1.0f) == 0.0f);
    // Normalising a -9 LUFS master to -14 is 5 dB down.
    CHECK(export_plan::plannedGainDb(true, -9.0f, -14.0f, -0.5f, -1.0f) == Catch::Approx(-5.0f).margin(0.01));

    // A -10 LUFS master measured against EBU R128 (-23 +/-1 LU, -1 dBTP)
    // fails as measured, but the export's -13 dB gain lands it on target.
    ipc::HttpServiceBridge::DeliveryTargetResult measured;
    measured.overall = "fail";
    ipc::HttpServiceBridge::DeliveryCheck integrated;
    integrated.id = "integrated";
    integrated.status = "fail";
    integrated.measured = -10.0;
    integrated.target = -23.0;
    ipc::HttpServiceBridge::DeliveryCheck tp;
    tp.id = "true_peak";
    tp.status = "fail";
    tp.measured = -0.2;
    tp.target = -1.0;
    ipc::HttpServiceBridge::DeliveryCheck lra;
    lra.id = "loudness_range";
    lra.status = "pass";
    lra.measured = 6.0;
    lra.target = 20.0;
    measured.checks = {integrated, tp, lra};

    const auto predicted = export_plan::predictAfterGain(measured, -13.0, 1.0);
    CHECK(predicted.overall == "pass");
    CHECK(*predicted.checks[0].measured == Catch::Approx(-23.0));
    CHECK(*predicted.checks[1].measured == Catch::Approx(-13.2));
    CHECK(*predicted.checks[2].measured == Catch::Approx(6.0)); // LRA is gain-independent

    // Not enough gain: still a fail on the integrated window.
    CHECK(export_plan::predictAfterGain(measured, -5.0, 1.0).overall == "fail");
    // Without a tolerance the integrated window only warns.
    auto noTol = measured;
    noTol.checks = {integrated};
    CHECK(export_plan::predictAfterGain(noTol, -5.0, std::nullopt).overall == "warn");
    // Zero gain: the server's verdict stands untouched.
    CHECK(export_plan::predictAfterGain(measured, 0.0, 1.0).overall == "fail");
}

TEST_CASE("UIX-21: Source rate keeps the file's own rate; lossy caps at 48 kHz", "[uix][export]")
{
    using SR = AudioExportOptions::SampleRate;
    CHECK(AudioExportOptions::resolveOutputRate(SR::Source, 48000.0, false) == 48000);
    CHECK(AudioExportOptions::resolveOutputRate(SR::Source, 96000.0, false) == 96000);
    CHECK(AudioExportOptions::resolveOutputRate(SR::Source, 96000.0, true) == 48000);
    CHECK(AudioExportOptions::resolveOutputRate(SR::Source, 44100.0, true) == 44100);
    CHECK(AudioExportOptions::resolveOutputRate(SR::SR44100, 48000.0, false) == 44100);
    CHECK(AudioExportOptions::resolveOutputRate(SR::SR96000, 48000.0, true) == 48000);
    CHECK(AudioExportOptions{}.sampleRate == SR::Source);
}

TEST_CASE("UIX-19: chat errors say what failed, by kind", "[uix][chat]")
{
    ipc::BridgeError e;
    e.kind = ipc::BridgeError::Kind::Timeout;
    CHECK(chat_text::errorReply(e).containsIgnoreCase("too long"));
    e.kind = ipc::BridgeError::Kind::ServiceUnavailable;
    CHECK(chat_text::errorReply(e).containsIgnoreCase("couldn't reach"));
    e.kind = ipc::BridgeError::Kind::HttpError;
    e.http_status = 401;
    CHECK(chat_text::errorReply(e).contains("401"));
    CHECK_FALSE(chat_text::errorReply(e).containsIgnoreCase("couldn't reach"));
    e.http_status = 422;
    e.message = "message too long";
    const auto text = chat_text::errorReply(e);
    CHECK(text.contains("422"));
    CHECK(text.contains("message too long"));
}

TEST_CASE("UIX-19: a stream_error from a completed stream is worded by its kind", "[uix][chat]")
{
    CHECK(chat_text::streamErrorReply("upstream timed out").containsIgnoreCase("too long"));
    CHECK(chat_text::streamErrorReply("Error code: 429 - rate limit").contains("429"));
    CHECK(chat_text::streamErrorReply("HTTP 401 unauthorized").containsIgnoreCase("refused"));
    const auto generic = chat_text::streamErrorReply("model overloaded");
    CHECK(generic.containsIgnoreCase("cut off"));
    CHECK(generic.contains("model overloaded"));
}

TEST_CASE("UIX-19: the chain diff is keyed by position, so two EQs diff against their own partner",
          "[uix][chat]")
{
    using Spec = ipc::HttpServiceBridge::EffectSpec;
    Spec eqLow, eqHigh, eqHighNew, comp;
    eqLow.effect = eqHigh.effect = eqHighNew.effect = "PeakFilter";
    eqLow.parameters["gain_db"] = -3.0f;
    eqHigh.parameters["gain_db"] = 2.0f;
    eqHighNew.parameters["gain_db"] = 4.0f;
    comp.effect = "Compressor";

    const auto diff = chat_text::stageDiff({eqLow, eqHigh}, {eqLow, eqHighNew, comp});
    CHECK(diff.contains("Stage 2 PeakFilter: gain_db (2.00 -> 4.00)"));
    CHECK_FALSE(diff.contains("Stage 1")); // stage 1 is unchanged
    CHECK(diff.contains("[+] Stage 3: Compressor"));
    CHECK(chat_text::stageDiff({eqLow}, {eqLow}).isEmpty());
    CHECK(chat_text::stageDiff({comp}, {eqLow}).contains("[~] Stage 1: Compressor -> PeakFilter"));
}

TEST_CASE("UIX-12: platform playback gain never turns up where the platform doesn't, nor past the ceiling",
          "[uix][streaming]")
{
    const auto& targets = getPlatformLoudnessTargets();
    auto find = [&](const char* name) -> const PlatformLoudnessTarget& {
        for (const auto& t : targets)
            if (t.name == name) return t;
        FAIL("platform missing");
        return targets.front();
    };
    const auto& spotify = find("Spotify");
    const auto& youtube = find("YouTube");
    const auto& apple = find("Apple Music");

    // Loud mixes are turned down everywhere.
    CHECK(platformPlaybackGainDb(youtube, -8.0f, -0.3f) == Catch::Approx(-6.0f));
    // A quiet -20 LUFS mix: YouTube / Apple never turn up.
    CHECK(platformPlaybackGainDb(youtube, -20.0f, -6.0f) == 0.0f);
    CHECK(platformPlaybackGainDb(apple, -20.0f, -6.0f) == 0.0f);
    // Spotify turns up, but only within its peak headroom (-1 dBTP).
    CHECK(platformPlaybackGainDb(spotify, -20.0f, -6.0f) == Catch::Approx(5.0f));
    CHECK(platformPlaybackGainDb(spotify, -20.0f, -10.0f) == Catch::Approx(6.0f));
    CHECK(platformPlaybackGainDb(spotify, -20.0f, -0.5f) == 0.0f);
    // Silence is not boosted 56 dB.
    CHECK(platformPlaybackGainDb(spotify, -120.0f, -120.0f) == 0.0f);
}

TEST_CASE("UIX-11: take alignment sends one fresh output path per take", "[uix][vocal]")
{
    TakeAlignmentRequest req;
    req.referencePath = "C:/s/vocals.wav";
    req.takePaths = {"C:/t/a.wav", "C:/t/b.wav"};
    CHECK(req.toVar()["output_paths"].isVoid()); // none chosen: server default

    req.outputPaths = {"C:/t/a_aligned_1.wav", "C:/t/b_aligned_1.wav"};
    const auto v = req.toVar();
    REQUIRE(v["output_paths"].isArray());
    CHECK(v["output_paths"].size() == 2);
    CHECK(v["output_paths"][1].toString() == "C:/t/b_aligned_1.wav");

    req.outputPaths = {"C:/t/a_aligned_1.wav"}; // mismatched count is never sent
    CHECK(req.toVar()["output_paths"].isVoid());
}

