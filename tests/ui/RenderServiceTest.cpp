// tests/ui/RenderServiceTest.cpp
// Pins RenderService (ENG-1 stage C2, extracted from AppController): the
// applied-render history (tree, undo/redo, describe), the AUDIT-002 session
// check, the UIX-01 stems-directory refusal, derived-render parenting and the
// session reset - without an AppController. Background work runs inline.
#include <catch2/catch_test_macros.hpp>
#include "IBackgroundWork.h"
#include "session/PlaybackService.h"
#include "session/RenderService.h"
#include "session/SessionService.h"
#include "support/NullServices.h"
#include "support/ScopedTempDir.h"

using khensu::PlaybackService;
using khensu::RenderService;
using khensu::SessionService;

namespace {
struct InlineWork final : khensu::IBackgroundWork {
    void runInBackground(std::function<void()> job) override { job(); }
    void runLongInBackground(std::function<void()> job) override { job(); }
    uint64_t sessionEpoch() const noexcept override { return 0; }
    bool isShuttingDown() const noexcept override { return false; }
};

/// Stems live in a fixed directory so the UIX-01 guard has something to refuse.
struct StemsInDir final : khensu::test::NullStems {
    juce::File dir;
    juce::File getStemsDirectory() const override { return dir; }
};

struct Fixture {
    khensu::test::ScopedTempDir tmp{"khensu-render-service-"}; // first: deleted after the services close khensu.db
    juce::File dir = tmp.file;
    InlineWork work;
    SessionService sessions;
    StemsInDir stems;
    khensu::test::NullAnalysisHub analysis;
    std::unique_ptr<PlaybackService> playback;
    std::unique_ptr<RenderService> renders;
    juce::File original;
    int treeChanges = 0;
    int acceptedChanges = 0;

    Fixture()
    {
        dir.createDirectory();
        stems.dir = dir.getChildFile("stems_test");
        stems.dir.createDirectory();
        original = writeSine("original.wav");
        sessions.attachStore(
            SessionService::openStore(dir.getChildFile("khensu.db").getFullPathName().toStdString(), 1).manager);
        playback = std::make_unique<PlaybackService>(work, sessions, stems, analysis);
        renders = std::make_unique<RenderService>(work, sessions, *playback, stems, analysis);
        renders->onAcceptedRenderChanged = [this] { ++acceptedChanges; };
        renders->addRenderTreeListener(this, [this] { ++treeChanges; });
    }
    ~Fixture()
    {
        renders->removeRenderTreeListener(this);
        renders.reset();
        playback.reset();
    }

    juce::File writeSine(const juce::String& name, const juce::File& parent = {}) const
    {
        const auto file = (parent == juce::File() ? dir : parent).getChildFile(name);
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream = file.createOutputStream();
        REQUIRE(stream != nullptr);
        const auto options =
            juce::AudioFormatWriterOptions{}.withSampleRate(44100.0).withNumChannels(2).withBitsPerSample(16);
        auto writer = wav.createWriterFor(stream, options);
        REQUIRE(writer != nullptr);
        juce::AudioBuffer<float> buf(2, 4410);
        for (int i = 0; i < buf.getNumSamples(); ++i) {
            const float v = 0.25f * std::sin(juce::MathConstants<float>::twoPi * 440.0f * (float)i / 44100.0f);
            buf.setSample(0, i, v);
            buf.setSample(1, i, v);
        }
        writer->writeFromAudioSampleBuffer(buf, 0, buf.getNumSamples());
        return file;
    }

    juce::String openSession()
    {
        khensu::AudioFileRef ref;
        ref.id = "f";
        ref.path = original.getFullPathName().toStdString();
        ref.format = "wav";
        auto created = sessions.create("t", {ref}, khensu::CreativeTarget{});
        REQUIRE(created.has_value());
        sessions.adopt(*created);
        playback->loadAudioFileForPlayback(original);
        return sessions.activeSessionId();
    }
};
} // namespace

TEST_CASE("RenderService: applied renders build the tree and walk undo/redo", "[split][renders]")
{
    Fixture f;
    f.openSession();
    const auto r1 = f.writeSine("r1.wav").getFullPathName();
    const auto r2 = f.writeSine("r2.wav").getFullPathName();

    CHECK(f.renders->describeRenderAt(0) == "original mix");
    f.renders->registerAppliedRender(r1);
    f.renders->registerAppliedRender(r2);
    REQUIRE(f.renders->renderTree().size() == 2);
    CHECK(f.renders->renderTree()[1].parent == -1); // every fix renders the original (AUDIT-064)
    CHECK(f.renders->currentRenderTreeNode() == 1);
    CHECK(f.renders->currentRenderPath() == r2);
    CHECK(f.renders->exportSourcePath() == r2);
    CHECK(f.playback->abProcessedPath() == r2); // the B side follows the history
    CHECK(f.treeChanges == 2);
    CHECK(f.acceptedChanges == 2);
    CHECK(f.renders->describeRenderAt(0) == "r2.wav");
    CHECK(f.renders->describeRenderAt(-1) == "r1.wav");
    CHECK(f.renders->describeRenderAt(1).isEmpty());

    REQUIRE(f.renders->canUndoRender());
    f.renders->undoRender();
    CHECK(f.renders->currentRenderPath() == r1);
    CHECK(f.playback->abProcessedPath() == r1);
    CHECK(f.renders->canRedoRender());
    f.renders->undoRender();
    CHECK(f.renders->currentRenderPath().isEmpty());
    CHECK(f.renders->exportSourcePath() == f.original.getFullPathName());
    CHECK_FALSE(f.renders->canUndoRender());
    f.renders->redoRender();
    f.renders->redoRender();
    CHECK(f.renders->currentRenderPath() == r2);
    CHECK(f.acceptedChanges == 6);
    // Undo/redo move the current node, so the tree panel must hear about each
    // (it kept highlighting the old node).
    CHECK(f.treeChanges == 6);

    // Re-registering a known path restores its node instead of duplicating it.
    f.renders->registerAppliedRender(r1);
    CHECK(f.renders->renderTree().size() == 2);
    CHECK(f.renders->currentRenderTreeNode() == 0);
    f.renders->restoreRenderTreeNode(1);
    CHECK(f.renders->currentRenderPath() == r2);
    f.renders->restoreRenderTreeNode(7); // out of range: no-op
    CHECK(f.renders->currentRenderPath() == r2);
    CHECK(f.renders->getAppliedRenderHistory().size() == 4);
}

TEST_CASE("RenderService drops a render started for another session (AUDIT-002)", "[split][renders]")
{
    Fixture f;
    const auto sessionId = f.openSession();
    const auto r1 = f.writeSine("r1.wav").getFullPathName();

    f.renders->registerAppliedRender(r1, "some-other-session");
    f.renders->registerAppliedRender(r1, {});
    f.renders->registerDerivedRender(r1, "some-other-session");
    CHECK(f.renders->renderTree().empty());
    CHECK(f.renders->currentRenderPath().isEmpty());
    CHECK(f.treeChanges == 0);
    CHECK(juce::File(r1).existsAsFile()); // rejected, never deleted

    f.renders->registerAppliedRender(r1, sessionId);
    CHECK(f.renders->currentRenderPath() == r1);
}

TEST_CASE("RenderService refuses a stem render as the mix render (UIX-01)", "[split][renders]")
{
    Fixture f;
    const auto sessionId = f.openSession();
    const auto stemRender = f.writeSine("vocals_pitch.wav", f.stems.dir).getFullPathName();

    CHECK(RenderService::isPathInsideDirectory(juce::File(stemRender), f.stems.dir));
    CHECK_FALSE(RenderService::isPathInsideDirectory(juce::File(stemRender), juce::File()));
    f.renders->registerAppliedRender(stemRender);
    f.renders->registerAppliedRender(stemRender, sessionId);
    f.renders->registerDerivedRender(stemRender, sessionId);
    CHECK(f.renders->renderTree().empty());
    CHECK(f.renders->currentRenderPath().isEmpty());
    // A missing file is ignored the same way.
    f.renders->registerAppliedRender(f.dir.getChildFile("missing.wav").getFullPathName());
    CHECK(f.renders->renderTree().empty());
}

TEST_CASE("RenderService parents a derived render at the current node (CE-AG-1)", "[split][renders]")
{
    Fixture f;
    const auto sessionId = f.openSession();
    const auto a = f.writeSine("a.wav").getFullPathName();
    const auto b = f.writeSine("b.wav").getFullPathName();
    const auto c = f.writeSine("c.wav").getFullPathName();

    f.renders->registerAppliedRender(a, sessionId);
    f.renders->registerDerivedRender(b, sessionId);
    f.renders->registerAppliedRender(c, sessionId);
    REQUIRE(f.renders->renderTree().size() == 3);
    CHECK(f.renders->renderTree()[0].parent == -1);
    CHECK(f.renders->renderTree()[1].parent == 0); // rendered from a.wav
    CHECK(f.renders->renderTree()[2].parent == -1);
    CHECK(f.renders->currentRenderPath() == c);
    CHECK(f.treeChanges == 3);
}

TEST_CASE("RenderService forgets the session's renders on reset", "[split][renders]")
{
    Fixture f;
    f.openSession();
    f.renders->registerAppliedRender(f.writeSine("r1.wav").getFullPathName());
    REQUIRE_FALSE(f.renders->renderTree().empty());

    f.renders->resetForSession();
    CHECK(f.renders->renderTree().empty());
    CHECK(f.renders->getAppliedRenderHistory().empty());
    CHECK(f.renders->currentRenderPath().isEmpty());
    CHECK(f.renders->currentRenderTreeNode() == -1);
    CHECK_FALSE(f.renders->canUndoRender());
    CHECK(f.renders->renderJobs().activeJobs().empty());

    // Without analysis the report entry points fail fast, on this thread.
    bool reported = false, ok = true;
    f.renders->generateReport(f.dir.getChildFile("r.html").getFullPathName(), "engineer",
                              [&](bool success, juce::String) {
                                  reported = true;
                                  ok = success;
                              });
    CHECK(reported);
    CHECK_FALSE(ok);
    // Without a session nothing renders.
    f.sessions.state().session.reset();
    bool deltaOk = true;
    f.renders->renderDeltaSolo({}, [&](bool success, juce::String) { deltaOk = success; });
    CHECK_FALSE(deltaOk);
}

TEST_CASE("RenderService renders the current version so fixes stack (WF-1)", "[split][renders]")
{
    Fixture f;
    const auto sessionId = f.openSession();
    using Start = khensu::IRenders::RenderStart;

    // Nothing accepted yet: the original.
    CHECK(f.renders->renderStart() == Start::CurrentVersion);
    CHECK(juce::File(f.renders->renderInputPath()) == f.original);

    const auto fix1 = f.writeSine("fix1.wav").getFullPathName();
    f.renders->registerRenderOf(fix1, f.original.getFullPathName(), sessionId);
    REQUIRE(f.renders->currentRenderPath() == fix1);
    CHECK(f.renders->renderTree()[0].parent == -1); // a render of the original is a root
    CHECK(f.renders->renderInputPath() == fix1);    // the next render stacks on it

    const auto fix2 = f.writeSine("fix2.wav").getFullPathName();
    f.renders->registerRenderOf(fix2, fix1, sessionId);
    REQUIRE(f.renders->renderTree().size() == 2);
    CHECK(f.renders->renderTree()[1].parent == 0); // fix 2 carries fix 1
    CHECK(f.renders->renderInputPath() == fix2);

    // "Start from original" - and every control hears about it.
    const int changesBefore = f.treeChanges;
    f.renders->setRenderStart(Start::Original);
    CHECK(f.treeChanges == changesBefore + 1);
    CHECK(juce::File(f.renders->renderInputPath()) == f.original);
    f.renders->setRenderStart(Start::Original); // no change, no notification
    CHECK(f.treeChanges == changesBefore + 1);

    // An accepted render gone from disk falls back to the original.
    f.renders->setRenderStart(Start::CurrentVersion);
    juce::File(fix2).deleteFile();
    CHECK(juce::File(f.renders->renderInputPath()) == f.original);

    // A render for another session is dropped; a session reset starts stacking again.
    f.renders->registerRenderOf(f.writeSine("late.wav").getFullPathName(), fix1, "other-session");
    CHECK(f.renders->renderTree().size() == 2);
    f.renders->setRenderStart(Start::Original);
    f.renders->resetForSession();
    CHECK(f.renders->renderStart() == Start::CurrentVersion);
}

namespace {
using Outcome = khensu::IRenders::RenderOutcome;

struct Reported {
    int calls = 0;
    Outcome outcome = Outcome::Failed;
    juce::String text;
};

khensu::IRenders::RenderDone recordInto(Reported& r)
{
    return [&r](Outcome o, const juce::String& text, const juce::var&) {
        ++r.calls;
        r.outcome = o;
        r.text = text;
    };
}

khensu::IRenders::ChainRender gainChain()
{
    khensu::IRenders::ChainRender render;
    render.description = "Test chain";
    render.chain.push_back({"Gain", {{"gain_db", -1.0f}}});
    return render;
}

juce::uint32 lastJobId(RenderService& renders)
{
    const auto jobs = renders.renderJobs().activeJobs();
    REQUIRE_FALSE(jobs.empty());
    return jobs.back().localId;
}
} // namespace

TEST_CASE("RenderService: renderVersion is the one completion path for a new version (REF-3)", "[split][renders]")
{
    Fixture f;
    const auto sessionId = f.openSession();

    SECTION("a finished render becomes a version of what it processed")
    {
        Reported r;
        const juce::File out = f.renders->renderVersion(gainChain(), "_test", recordInto(r));
        CHECK(out.getFileName().startsWith("original_test"));
        CHECK(out.getParentDirectory() == f.original.getParentDirectory());
        CHECK(f.renders->renderJobs().activeJobs().back().description == "Test chain -> " + out.getFileName());

        const auto rendered = f.writeSine(out.getFileName()).getFullPathName();
        f.renders->renderJobs().finishJobForTesting(lastJobId(*f.renders), true, rendered);
        CHECK(r.calls == 1);
        CHECK(r.outcome == Outcome::Rendered);
        CHECK(r.text == rendered);
        CHECK(f.renders->currentRenderPath() == rendered);
        CHECK(f.renders->renderTree()[0].parent == -1); // processed the original: a root
    }
    SECTION("a render finishing after the session changed is reported and dropped (AUDIT-002)")
    {
        Reported r;
        f.renders->renderVersion(gainChain(), "_test", recordInto(r));
        const auto id = lastJobId(*f.renders);
        f.openSession(); // the user moved to another session
        f.renders->renderJobs().finishJobForTesting(id, true, f.writeSine("late.wav").getFullPathName());
        CHECK(r.outcome == Outcome::SessionChanged);
        CHECK(f.renders->renderTree().empty());
    }
    SECTION("cancel and failure are told apart")
    {
        Reported cancelled, failed;
        f.renders->renderVersion(gainChain(), "_a", recordInto(cancelled));
        f.renders->renderJobs().cancelAll();
        CHECK(cancelled.outcome == Outcome::Cancelled);

        f.renders->renderVersion(gainChain(), "_b", recordInto(failed));
        f.renders->renderJobs().finishJobForTesting(lastJobId(*f.renders), false, "disk full");
        CHECK(failed.outcome == Outcome::Failed);
        CHECK(failed.text == "disk full");
        CHECK(f.renders->renderTree().empty());
    }
    SECTION("nothing to render starts nothing")
    {
        Reported r;
        CHECK(f.renders->renderVersion(khensu::IRenders::ChainRender{}, "_x", recordInto(r)) == juce::File());
        CHECK(r.calls == 1);
        CHECK(r.outcome == Outcome::Failed);
        CHECK(f.renders->renderJobs().activeJobs().empty());
    }
    juce::ignoreUnused(sessionId);
}

TEST_CASE("RenderService: renderPreview never registers a version (REF-3)", "[split][renders]")
{
    Fixture f;
    f.openSession();
    Reported r;
    const auto preview = f.dir.getChildFile("preview.wav").getFullPathName();
    f.renders->renderPreview(gainChain(), f.original.getFullPathName(), preview, recordInto(r));
    f.renders->renderJobs().finishJobForTesting(lastJobId(*f.renders), true, f.writeSine("preview.wav").getFullPathName());
    CHECK(r.outcome == Outcome::Rendered);
    CHECK(f.renders->renderTree().empty());
    CHECK(f.renders->currentRenderPath().isEmpty());
}

TEST_CASE("RenderService: the Done entry points word every outcome (REF-3)", "[split][renders]")
{
    Fixture f;
    f.openSession();
    std::vector<std::pair<bool, juce::String>> done;
    const auto record = [&done](bool ok, juce::String text) { done.emplace_back(ok, text); };

    f.renders->applyEffectChain(gainChain().chain, "tag", record);
    f.renders->renderJobs().cancelAll();
    REQUIRE(done.size() == 1);
    CHECK_FALSE(done[0].first);
    CHECK(done[0].second == "Render cancelled.");

    // applyCleanupChain used to report a dropped render (window closed,
    // session changed) as whatever the job said; it now matches the others.
    f.renders->applyCleanupChain("Declip", 1, record);
    const auto id = lastJobId(*f.renders);
    f.openSession();
    f.renders->renderJobs().finishJobForTesting(id, true, f.writeSine("clean.wav").getFullPathName());
    REQUIRE(done.size() == 2);
    CHECK_FALSE(done[1].first);
    CHECK(done[1].second == "Session changed - the render was not loaded.");
}

// ---- OVH-9: renders without the service ------------------------------------

namespace {
/// Pumps the message loop (RenderJobManager posts its results with callAsync).
template <typename Pred> bool pumpUntil(Pred pred, int timeoutMs = 20000)
{
    auto* mm = juce::MessageManager::getInstance();
    const auto deadline = juce::Time::getMillisecondCounter() + static_cast<juce::uint32>(timeoutMs);
    while (!pred() && juce::Time::getMillisecondCounter() < deadline) mm->runDispatchLoopUntil(20);
    return pred();
}

khensu::IRenders::ChainRender shelfChain()
{
    khensu::IRenders::ChainRender render;
    render.description = "Low shelf";
    render.chain.push_back({"LowShelfFilter", {{"cutoff_frequency_hz", 120.0f}, {"gain_db", -3.0f}, {"q", 0.707f}}});
    render.chain.push_back({"Limiter", {{"threshold_db", -1.0f}}});
    return render;
}
} // namespace

TEST_CASE("RenderService: the native fallback decision (OVH-9)", "[split][renders][ovh9]")
{
    using Spec = khensu::ipc::HttpServiceBridge::EffectSpec;
    const std::vector<Spec> native = {{"HighpassFilter", {{"cutoff_frequency_hz", 30.0f}}},
                                      {"LowShelfFilter", {{"gain_db", -3.0f}}},
                                      {"PeakFilter", {{"gain_db", -6.0f}, {"q", 6.0f}}},
                                      {"Compressor", {{"ratio", 2.0f}}},
                                      {"DeEsser", {{"threshold_db", -22.0f}}},
                                      {"Gain", {{"gain_db", 2.0f}}},
                                      {"Limiter", {{"threshold_db", -1.0f}}}};
    CHECK(RenderService::shouldRenderNatively(false, native));
    CHECK_FALSE(RenderService::shouldRenderNatively(true, native)); // the service is the reference when up
    // FN2-2a: every effect a fix emits renders natively now.
    for (const char* nowNative : {"SpectralDenoise", "TransientShaper", "TapeSaturation", "StereoWidener",
                                   "DynamicEQ", "MultibandCompressor", "MidSideProcessor", "Exciter",
                                   "BassMonoMaker", "NoiseGate"}) {
        auto chain = native;
        chain.push_back({nowNative, {}});
        INFO(nowNative);
        CHECK(RenderService::shouldRenderNatively(false, chain));
    }
    for (const char* engineOnly : {"Reverb", "GlueCompressor"}) {
        auto chain = native;
        chain.push_back({engineOnly, {}});
        INFO(engineOnly);
        CHECK_FALSE(RenderService::shouldRenderNatively(false, chain));
    }
    CHECK_FALSE(RenderService::shouldRenderNatively(false, {}));
    const auto converted = RenderService::nativeChain(native);
    REQUIRE(converted.size() == native.size());
    CHECK(converted[2].effect == "PeakFilter");
    CHECK(converted[2].parameters.at("q") == 6.0f);
}

TEST_CASE("RenderService renders a fix natively while the engine is down (OVH-9)", "[split][renders][ovh9]")
{
    Fixture f;
    const auto sessionId = f.openSession();
    bool serviceUp = false;
    f.renders->serviceAvailable = [&serviceUp] { return serviceUp; };

    SECTION("it becomes a version, exactly like a service render")
    {
        Reported r;
        juce::var result;
        const auto out = f.renders->renderVersion(shelfChain(), "_fix",
                                                  [&](Outcome o, const juce::String& text, const juce::var& full) {
                                                      ++r.calls;
                                                      r.outcome = o;
                                                      r.text = text;
                                                      result = full;
                                                  });
        REQUIRE(f.renders->renderJobs().activeJobs().size() == 1);
        CHECK(f.renders->renderJobs().activeJobs().front().status == "running locally");
        REQUIRE(pumpUntil([&] { return r.calls > 0; }));
        INFO(r.text);
        REQUIRE(r.outcome == Outcome::Rendered);
        CHECK(r.text == out.getFullPathName());
        CHECK(out.existsAsFile());
        CHECK(f.renders->currentRenderPath() == out.getFullPathName());
        CHECK(f.renders->renderTree()[0].parent == -1);
        CHECK(f.playback->abProcessedPath() == out.getFullPathName());
        CHECK(result["renderer"].toString() == "native");
        CHECK(static_cast<bool>(result["guardrail"]["ok"]));
        CHECK(result["applied_effects"].size() == 2);
        CHECK(f.renders->renderJobs().lastChain().size() == 2); // the preset library's "last applied chain"
        CHECK(f.renders->renderJobs().activeJobs().empty());

        // The next fix stacks on it (WF-1), still natively.
        Reported second;
        f.renders->renderVersion(shelfChain(), "_fix", recordInto(second));
        REQUIRE(pumpUntil([&] { return second.calls > 0; }));
        REQUIRE(second.outcome == Outcome::Rendered);
        REQUIRE(f.renders->renderTree().size() == 2);
        CHECK(f.renders->renderTree()[1].parent == 0);
    }
    SECTION("a cancelled native render reports Cancelled and leaves no file")
    {
        Reported r;
        const auto out = f.renders->renderVersion(shelfChain(), "_fix", recordInto(r));
        f.renders->renderJobs().cancelAll();
        CHECK(r.outcome == Outcome::Cancelled);
        pumpUntil([] { return false; }, 1500); // the worker finishes; its late result is discarded
        CHECK_FALSE(out.existsAsFile());
        CHECK(f.renders->renderTree().empty());
    }
    SECTION("a native render finishing after the session changed is not registered (AUDIT-002)")
    {
        Reported r;
        f.renders->renderVersion(shelfChain(), "_fix", recordInto(r));
        f.openSession(); // another session is active when it lands
        REQUIRE(pumpUntil([&] { return r.calls > 0; }));
        CHECK(r.outcome == Outcome::SessionChanged);
        CHECK(f.renders->renderTree().empty());
    }
    SECTION("an engine-only chain still goes to the service")
    {
        auto render = shelfChain();
        render.chain.push_back({"Reverb", {}});
        Reported r;
        f.renders->renderVersion(render, "_fix", recordInto(r));
        REQUIRE(f.renders->renderJobs().activeJobs().size() == 1);
        CHECK(f.renders->renderJobs().activeJobs().front().status != "running locally");
        f.renders->renderJobs().cancelAll();
        CHECK(r.outcome == Outcome::Cancelled);
    }
    SECTION("with the engine up the service renders")
    {
        serviceUp = true;
        Reported r;
        f.renders->renderVersion(shelfChain(), "_fix", recordInto(r));
        REQUIRE(f.renders->renderJobs().activeJobs().size() == 1);
        CHECK(f.renders->renderJobs().activeJobs().front().status == "starting");
        f.renders->renderJobs().cancelAll();
    }
    juce::ignoreUnused(sessionId);
}
