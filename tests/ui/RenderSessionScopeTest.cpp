// tests/ui/RenderSessionScopeTest.cpp
// Regression tests for the desktop render / A-B / export slice of the
// 2026-10-15 audit: what Export writes (AUDIT-001), renders finishing after a
// session switch (AUDIT-002), same-path re-renders and the A/B loudness hold
// (AUDIT-018 / -038), source-switch fades (AUDIT-037), the WAV-only 32-bit
// export (AUDIT-041) and the chat-history window (AUDIT-043). AppController
// itself needs the whole app to link, so these drive the seams it delegates
// to (RenderHistory, PlaybackSourceController, LufsCache, RenderJobManager,
// PlaybackEngine::FadeGate) and the panels that run against IAppController.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include "RenderJobManager.h"
#include "export/AudioExporter.h"
#include "session/LufsCache.h"
#include "session/PlaybackEngine.h"
#include "session/PlaybackSourceController.h"
#include "session/RenderHistory.h"
#include "ui/Copilot/ChatHistoryWindow.h"
#include "ui/Copilot/MasteringAgentPanel.h"
#include "ui/Copilot/SectionIntensityPanel.h"
#include "support/FakeAppController.h"
#include "support/FakeApplyChainServer.h"
#include <khensu/analysis/AnalysisResult.h>
#include <khensu/analysis/SectionAnalysis.h>
#include <khensu/session/Session.h>
#include <algorithm>
#include <cmath>

using namespace khensu;
using khensu::test::FakeApplyChainServer;
using khensu::test::FakeAppController;

namespace {

template <typename Pred> bool pumpUntil(Pred pred, int timeoutMs = 10000)
{
    auto* mm = juce::MessageManager::getInstance();
    const auto deadline = juce::Time::currentTimeMillis() + timeoutMs;
    while (!pred()) {
        if (juce::Time::currentTimeMillis() > deadline) return false;
        mm->runDispatchLoopUntil(20);
    }
    return true;
}

/// A scratch directory that is removed again at scope exit.
struct TempDir {
    juce::File dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                         .getChildFile("khensu-c1-" + juce::String::toHexString(juce::Random::getSystemRandom().nextInt()));
    TempDir() { dir.createDirectory(); }
    ~TempDir() { dir.deleteRecursively(); }
    juce::File file(const juce::String& name, const juce::String& content) const
    {
        const auto f = dir.getChildFile(name);
        f.replaceWithText(content);
        return f;
    }
};

Session sessionWithId(const juce::String& id)
{
    Session s;
    s.id = id.toStdString();
    s.name = "Test Session";
    AudioFileRef f;
    f.path = "Z:/does/not/exist.wav";
    s.files.push_back(f);
    return s;
}

AnalysisResult analysisWithOneSection()
{
    AnalysisResult a;
    a.genre_relative_action_allowed = true;
    SectionAnalysis sa;
    SectionData sec;
    sec.label = "section_1";
    sec.start_sec = 0.0f;
    sec.end_sec = 30.0f;
    sa.sections.push_back(sec);
    sa.section_count = 1;
    a.section_analysis = sa;
    return a;
}

/// Constant full-scale input for the fade gate.
struct OnesSource final : juce::AudioSource {
    void prepareToPlay(int, double) override {}
    void releaseResources() override {}
    void getNextAudioBlock(const juce::AudioSourceChannelInfo& info) override
    {
        for (int ch = 0; ch < info.buffer->getNumChannels(); ++ch)
            juce::FloatVectorOperations::fill(info.buffer->getWritePointer(ch, info.startSample), 1.0f,
                                              info.numSamples);
    }
};

struct Turn {
    juce::String role, content;
};

} // namespace

// ---- AUDIT-001 ------------------------------------------------------------

TEST_CASE("Export source is the accepted render, never a blind reference in the B slot", "[audit001][export]")
{
    TempDir tmp;
    const auto original = tmp.file("song.wav", "original");
    const auto render = tmp.file("song_eq_1.wav", "render");
    const auto reference = tmp.file("someone_elses_master.wav", "reference");

    RenderHistory history;
    PlaybackSourceController ab;
    history.registerRender(render.getFullPathName());
    ab.assignProcessed(render.getFullPathName(), {}, std::nullopt, true);

    // An ABX trial borrows the B slot for a reference (and a chat audition or
    // an agent candidate would do the same) - the export must not follow it.
    ab.assignProcessed(reference.getFullPathName(), {}, std::nullopt, true);
    CHECK(ab.processedPath == reference.getFullPathName());
    CHECK(history.exportSource(original.getFullPathName()) == render.getFullPathName());

    // Back on the original (undo), or the render deleted from disk: original.
    history.undo();
    CHECK(history.exportSource(original.getFullPathName()) == original.getFullPathName());
    history.redo();
    REQUIRE(render.deleteFile());
    CHECK(history.exportSource(original.getFullPathName()) == original.getFullPathName());
}

// ---- AUDIT-002 ------------------------------------------------------------

TEST_CASE("RenderJobManager::cancelAll cancels every session-scoped render", "[audit002][renderjobmanager]")
{
    FakeApplyChainServer server(19741);
    server.setResponse("POST /copilot/apply_chain/start", {200, R"({"job_id":"job-a"})"});
    server.setResponse("GET /copilot/apply_chain/status/job-a", {200, R"({"status":"running"})"});

    // Own the message thread BEFORE any pool thread posts: a callAsync from a
    // worker would otherwise create the MessageManager on that worker, and
    // this thread's pumping would never dispatch the /start result.
    juce::MessageManager::getInstance();
    RenderJobManager mgr(server.baseUrl().toStdString());
    int completedOk = 0, cancelled = 0, failed = 0;
    juce::String lastError;
    const auto cb = [&](bool ok, const juce::String& err, const juce::var&) {
        if (ok) ++completedOk;
        else if (err.isEmpty()) ++cancelled;
        else {
            ++failed;
            lastError = err;
        }
    };
    mgr.startChainRender("A1", "Z:/a.wav", "Z:/a_out1.wav", {}, "sessionA", "r1", cb);
    const bool started = pumpUntil([&] {
        return failed > 0 || (!mgr.activeJobs().empty() && mgr.activeJobs()[0].jobId.isNotEmpty());
    });
    REQUIRE(started);
    CHECK(lastError.toStdString() == std::string{});
    REQUIRE(failed == 0);
    mgr.startChainRender("A2", "Z:/a.wav", "Z:/a_out2.wav", {}, "sessionA", "r2", cb); // still "starting"
    REQUIRE(mgr.activeJobs().size() == 2);

    mgr.cancelAll(); // what AppController::resetSessionScopedState() does on a session switch

    CHECK(mgr.activeJobs().empty());
    CHECK(cancelled == 2);
    CHECK(completedOk == 0);
    REQUIRE(pumpUntil([&] { return server.requestCount("POST /copilot/apply_chain/job-a/cancel") > 0; }, 5000));
}

TEST_CASE("A section render that completes after a session switch is cancelled, not registered",
          "[audit002][sectionintensitypanel]")
{
    FakeApplyChainServer server(19742);
    server.setResponse("POST /copilot/apply_section_chain/start", {200, R"({"job_id":"job-switch"})"});
    server.setResponse("GET /copilot/apply_section_chain/status/job-switch",
                       {200, R"({"status":"completed","result":{
                           "output_path":"Z:/sectioned.wav", "guardrail":{"ok":true}, "sections":[]
                       }})"});

    FakeAppController fake(server.baseUrl());
    fake.session = sessionWithId("songA");
    fake.analysisResult = analysisWithOneSection();
    SectionIntensityPanel panel(fake, server.baseUrl().toStdString());
    panel.refreshSections();

    panel.apply();
    REQUIRE(pumpUntil([&] { return !panel.currentJobId().isEmpty() || panel.statusText().startsWith("Error"); }));
    REQUIRE(panel.currentJobId().isNotEmpty());

    fake.session = sessionWithId("songB"); // the user opens another song mid-render

    REQUIRE(pumpUntil([&] { return panel.currentJobId().isEmpty(); }, 15000));
    CHECK(fake.registerAppliedRenderCallCount == 0);
    CHECK(fake.rejectedRenderCallCount == 0); // never even offered
    CHECK(panel.statusText().containsIgnoreCase("session changed"));
    CHECK(server.requestCount("POST /copilot/apply_section_chain/job-switch/cancel") == 1);
    CHECK(panel.isApplyButtonEnabled());
}

TEST_CASE("A mastering candidate from a superseded session never reaches the new session",
          "[audit002][masteringagentpanel]")
{
    FakeApplyChainServer server(19743);
    server.setResponse("POST /copilot/mastering_agent/start", {200, R"({"job_id":"job-m"})"});
    server.setResponse("GET /copilot/mastering_agent/status/job-m",
                       {200, R"({"status":"completed","result":{
                           "converged": true, "requires_user_choice": false,
                           "final_output_path": "Z:/mastered.wav", "decision_hash": "abc",
                           "guardrail": {"ok": true}, "iterations": []
                       }})"});

    FakeAppController fake(server.baseUrl());
    fake.session = sessionWithId("songA");
    AnalysisResult a;
    a.genre_relative_action_allowed = true;
    fake.analysisResult = a;
    MasteringAgentPanel panel(fake, server.baseUrl().toStdString());

    panel.run();
    REQUIRE(pumpUntil([&] { return !panel.currentJobId().isEmpty() || panel.statusText().startsWith("Error"); }));
    REQUIRE(panel.currentJobId().isNotEmpty());
    fake.session = sessionWithId("songB");

    REQUIRE(pumpUntil([&] { return panel.currentJobId().isEmpty(); }, 15000));
    CHECK(fake.setAbProcessedPathCallCount == 0); // the candidate never became B's A/B side
    CHECK(server.requestCount("POST /copilot/mastering_agent/job-m/cancel") == 1);
    panel.acceptCandidate(); // nothing pending: a no-op
    CHECK(fake.registerAppliedRenderCallCount == 0);
}

TEST_CASE("A candidate accepted after switching sessions is rejected", "[audit002][masteringagentpanel]")
{
    FakeApplyChainServer server(19744);
    server.setResponse("POST /copilot/mastering_agent/start", {200, R"({"job_id":"job-n"})"});
    server.setResponse("GET /copilot/mastering_agent/status/job-n",
                       {200, R"({"status":"completed","result":{
                           "converged": true, "requires_user_choice": true,
                           "final_output_path": "Z:/mastered.wav", "decision_hash": "abc",
                           "guardrail": {"ok": true}, "iterations": []
                       }})"});

    FakeAppController fake(server.baseUrl());
    fake.session = sessionWithId("songA");
    AnalysisResult a;
    a.genre_relative_action_allowed = true;
    fake.analysisResult = a;
    MasteringAgentPanel panel(fake, server.baseUrl().toStdString());

    panel.run();
    REQUIRE(pumpUntil([&] { return !panel.currentJobId().isEmpty() || panel.statusText().startsWith("Error"); }));
    REQUIRE(pumpUntil([&] { return panel.currentJobId().isEmpty(); }, 15000));
    REQUIRE(fake.lastAbProcessedPath == "Z:/mastered.wav"); // candidate loaded for song A

    fake.session = sessionWithId("songB");
    panel.acceptCandidate();
    CHECK(fake.registerAppliedRenderCallCount == 0);
    CHECK(fake.enqueueProvenanceEventCallCount == 0);
    CHECK(panel.statusText().containsIgnoreCase("different session"));
}

// ---- AUDIT-018 / AUDIT-038 -------------------------------------------------

TEST_CASE("Setting the same B path again reloads and re-levels it", "[audit018][ab]")
{
    PlaybackSourceController ab;
    auto first = ab.assignProcessed("Z:/preview.wav", "Z:/song.wav", std::nullopt, true);
    CHECK_FALSE(first.reloadTransport); // A is loaded: nothing to reload
    CHECK(first.measure);
    ab.processedLufs = -12.0f;
    ab.showingProcessed = true; // user switches to B and it loads

    // The file is re-rendered IN PLACE under the same name and set again while
    // B is playing: the old early-return kept the stale audio + level.
    auto again = ab.assignProcessed("Z:/preview.wav", "Z:/preview.wav", std::nullopt, true);
    CHECK(again.reloadTransport);
    CHECK(again.measure);
    CHECK_FALSE(ab.processedLufs.has_value()); // old level dropped -> B held muted until re-measured
    CHECK(ab.awaitingProcessedLufs());

    // A known (cached) level is applied straight away, no measurement.
    auto cached = ab.assignProcessed("Z:/preview.wav", "Z:/preview.wav", -15.5f, true);
    CHECK(cached.reloadTransport);
    CHECK_FALSE(cached.measure);
    REQUIRE(ab.processedLufs.has_value());
    CHECK(*ab.processedLufs == Catch::Approx(-15.5f));

    // Clearing B while it plays falls back to A.
    auto cleared = ab.assignProcessed({}, "Z:/preview.wav", std::nullopt, false);
    CHECK(cleared.reloadTransport);
    CHECK_FALSE(ab.showingProcessed);
}

TEST_CASE("LufsCache forgets a level once the file is rewritten", "[audit018][audit038]")
{
    TempDir tmp;
    const auto f = tmp.file("render.wav", "first render");
    LufsCache cache;
    CHECK_FALSE(cache.find(f.getFullPathName()).has_value());
    cache.remember(f.getFullPathName(), -11.0f);
    REQUIRE(cache.find(f.getFullPathName()).has_value());
    CHECK(*cache.find(f.getFullPathName()) == Catch::Approx(-11.0f));

    f.replaceWithText("a different, longer second render"); // same path, new audio
    CHECK_FALSE(cache.find(f.getFullPathName()).has_value());

    cache.remember(tmp.dir.getChildFile("missing.wav").getFullPathName(), -9.0f); // no file: ignored
    CHECK_FALSE(cache.find(tmp.dir.getChildFile("missing.wav").getFullPathName()).has_value());
}

// ---- AUDIT-037 ------------------------------------------------------------

TEST_CASE("Playback fade gate ramps instead of cutting", "[audit037][playback]")
{
    OnesSource ones;
    PlaybackEngine::FadeGate gate(ones);
    constexpr double sr = 48000.0;
    gate.prepareToPlay(512, sr);
    const int fadeSamples = (int)std::lround(sr * PlaybackEngine::kFadeMs / 1000.0);
    REQUIRE(fadeSamples >= (int)(sr * 0.010)); // 10-30 ms per the audit fix
    REQUIRE(fadeSamples <= (int)(sr * 0.030));

    juce::AudioBuffer<float> buf(2, 512);
    std::vector<float> out;
    const auto run = [&](int blocks) {
        for (int b = 0; b < blocks; ++b) {
            gate.getNextAudioBlock(juce::AudioSourceChannelInfo(&buf, 0, 512));
            for (int i = 0; i < 512; ++i)
                out.push_back(buf.getSample(0, i));
        }
    };

    run(1);
    CHECK(out.back() == Catch::Approx(1.0f)); // unity at rest

    out.clear();
    gate.fadeTo(0.0f);
    run(4);
    CHECK(out.front() > 0.99f);
    CHECK(gate.reachedGain() == 0.0f);
    CHECK(out.back() == 0.0f);
    float maxStep = 0.0f;
    for (size_t i = 1; i < out.size(); ++i)
        maxStep = juce::jmax(maxStep, std::abs(out[i] - out[i - 1]));
    CHECK(maxStep <= 1.0f / (float)fadeSamples + 1.0e-4f); // no discontinuity anywhere
    // Silence is reached after ~kFadeMs, not instantly and not much later.
    const auto firstZero = std::find(out.begin(), out.end(), 0.0f) - out.begin();
    CHECK(firstZero >= fadeSamples - 2);
    CHECK(firstZero <= fadeSamples + 2);

    out.clear();
    gate.fadeTo(1.0f);
    run(4);
    CHECK(out.front() < 0.01f); // fades IN from silence
    CHECK(out.back() == Catch::Approx(1.0f));
}

// ---- AUDIT-041 ------------------------------------------------------------

TEST_CASE("32-bit export is offered only for WAV", "[audit041][export]")
{
    using F = AudioExportOptions::Format;
    using D = AudioExportOptions::BitDepth;
    CHECK(AudioExportOptions::supportsBitDepth(F::WAV, D::Bits32));
    CHECK_FALSE(AudioExportOptions::supportsBitDepth(F::FLAC, D::Bits32));
    CHECK_FALSE(AudioExportOptions::supportsBitDepth(F::AIFF, D::Bits32));
    for (const auto f : {F::WAV, F::AIFF, F::FLAC})
        for (const auto d : {D::Bits16, D::Bits24})
            CHECK(AudioExportOptions::supportsBitDepth(f, d));
    // Lossy encoders ignore the depth picker (always fed 16-bit PCM).
    CHECK(AudioExportOptions::supportsBitDepth(F::MP3, D::Bits32));
    CHECK(AudioExportOptions::supportsBitDepth(F::AAC, D::Bits32));

    // And the writers really do agree with the rule.
    CHECK(juce::WavAudioFormat().getPossibleBitDepths().contains(32));
    CHECK_FALSE(juce::FlacAudioFormat().getPossibleBitDepths().contains(32));
    CHECK_FALSE(juce::AiffAudioFormat().getPossibleBitDepths().contains(32));
}

// ---- AUDIT-043 ------------------------------------------------------------

TEST_CASE("Chat history is windowed under the service's 200-message cap", "[audit043][chat]")
{
    std::vector<Turn> turns;
    for (int i = 0; i < 250; ++i)
        turns.push_back({i % 2 == 0 ? "user" : "assistant", "message " + juce::String(i)});

    chat_history::trimToWindow(turns);
    CHECK(turns.size() <= chat_history::kMaxMessages);
    CHECK(turns.size() < 200);
    CHECK(turns.front().role == "user");          // the window opens on a user turn
    CHECK(turns.back().content == "message 249"); // newest kept

    // A few huge replies are dropped (oldest first) to stay in the char budget.
    std::vector<Turn> big;
    for (int i = 0; i < 10; ++i)
        big.push_back({i % 2 == 0 ? "user" : "assistant", juce::String::repeatedString("x", 30000)});
    chat_history::trimToWindow(big);
    long total = 0;
    for (const auto& t : big)
        total += juce::jmin(t.content.length(), chat_history::kMaxMessageChars);
    CHECK(total <= chat_history::kMaxTotalChars);
    CHECK_FALSE(big.empty());
    CHECK(chat_history::clipForSend(big.back().content).length() == chat_history::kMaxMessageChars);
}
