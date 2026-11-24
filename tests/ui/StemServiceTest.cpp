// tests/ui/StemServiceTest.cpp
// Pins StemService (ENG-1 stage C3, extracted from AppController): the stems
// directory derivation (AMAS_ naming, AUDIT-074 hash suffix, reuse of an
// existing folder), stemFilesExist / hasStemsAvailable against a real vs HPSS
// separation.json (AID-07), getStemFile for missing stems and the session
// reset - without an AppController. Background work is counted, not run.
#include <thread>
#include <catch2/catch_test_macros.hpp>
#include "IBackgroundWork.h"
#include "session/SessionService.h"
#include "session/StemService.h"
#include <khensu/ipc/HttpServiceBridge.h>

using khensu::SessionService;
using khensu::StemService;

namespace {
struct CountingWork final : khensu::IBackgroundWork {
    int jobs = 0;
    void runInBackground(std::function<void()>) override { ++jobs; }
    void runLongInBackground(std::function<void()>) override { ++jobs; }
    uint64_t sessionEpoch() const noexcept override { return 0; }
    bool isShuttingDown() const noexcept override { return false; }
};

struct Fixture {
    juce::File dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                         .getChildFile("khensu-stem-service-" + juce::Uuid().toString());
    CountingWork work;
    SessionService sessions;
    StemService stems{work, sessions};

    Fixture() { dir.createDirectory(); }
    ~Fixture() { dir.deleteRecursively(); }

    juce::File openSession(const juce::String& fileName)
    {
        const auto source = dir.getChildFile(fileName);
        source.replaceWithText("not audio"); // only the path matters here
        khensu::Session session;
        session.id = "session-1";
        session.name = "t";
        khensu::AudioFileRef ref;
        ref.id = "f";
        ref.path = source.getFullPathName().toStdString();
        session.files.push_back(ref);
        sessions.adopt(std::move(session));
        return source;
    }

    void writeStems(const juce::File& stemsDir, const char* method)
    {
        REQUIRE(stemsDir.createDirectory());
        for (const char* name : {"vocals", "drums", "bass", "other"})
            REQUIRE(stemsDir.getChildFile(juce::String(name) + ".wav").replaceWithText("x"));
        if (method != nullptr)
            REQUIRE(stemsDir.getChildFile(khensu::StemSeparationInfo::kFileName)
                        .replaceWithText(juce::String("{\"method\": \"") + method + "\", \"reason\": \"\"}"));
    }
};
} // namespace

TEST_CASE("StemService has no stems without a session", "[split][stems]")
{
    Fixture f;
    CHECK(f.stems.getStemsDirectory() == juce::File());
    CHECK_FALSE(f.stems.stemFilesExist());
    CHECK_FALSE(f.stems.hasStemsAvailable());
    CHECK_FALSE(f.stems.hasApproximateStems());
    CHECK_FALSE(f.stems.isSeparatingStems());
    CHECK_FALSE(f.stems.hasVocalAnalysis());

    bool called = false, ok = true;
    f.stems.runStemSeparation("demucs", [&](bool success, juce::String) {
        called = true;
        ok = success;
    });
    CHECK(called); // fails fast, on this thread
    CHECK_FALSE(ok);
    CHECK_FALSE(f.stems.isSeparatingStems());
    f.stems.ensureVocalAnalysis(); // nothing to analyse
    CHECK(f.work.jobs == 0);
}

TEST_CASE("StemService derives the AMAS_ stems directory next to the source", "[split][stems]")
{
    SECTION("plain title")
    {
        Fixture f;
        const auto source = f.openSession("My Song.wav");
        const auto stemsDir = f.stems.getStemsDirectory();
        CHECK(stemsDir.getParentDirectory() == source.getParentDirectory());
        CHECK(stemsDir.getFileName().startsWith("AMAS_"));
        CHECK(stemsDir.getFileName().endsWith("_MySong"));
        CHECK(f.stems.getStemFile("vocals") == stemsDir.getChildFile("vocals.wav"));
        CHECK_FALSE(f.stems.getStemFile("vocals").existsAsFile()); // not separated yet
    }
    SECTION("an existing folder for the title is reused")
    {
        Fixture f;
        const auto existing = f.dir.getChildFile("AMAS_1_2_2020_MySong");
        REQUIRE(existing.createDirectory());
        f.openSession("My Song.wav");
        CHECK(f.stems.getStemsDirectory() == existing);
    }
    SECTION("a lossy sanitisation gets a hash suffix (AUDIT-074)")
    {
        Fixture f;
        f.openSession(juce::String(juce::CharPointer_UTF8("Caf\xc3\xa9.wav")));
        const auto name = f.stems.getStemsDirectory().getFileName();
        CHECK(name.startsWith("AMAS_"));
        CHECK(name.contains("_Caf_"));
        CHECK(name.fromLastOccurrenceOf("_", false, false).length() == 8);
    }
}

TEST_CASE("StemService counts HPSS stems as files but not as real stems (AID-07)", "[split][stems]")
{
    Fixture f;
    f.openSession("song.wav");
    const auto stemsDir = f.stems.getStemsDirectory();

    SECTION("real separation")
    {
        f.writeStems(stemsDir, "demucs");
        CHECK(f.stems.stemFilesExist());
        CHECK(f.stems.hasStemsAvailable());
        CHECK_FALSE(f.stems.hasApproximateStems());
        CHECK(f.stems.stemSeparationInfo().method == "demucs");
    }
    SECTION("HPSS fallback")
    {
        f.writeStems(stemsDir, "hpss");
        CHECK(f.stems.stemFilesExist());
        CHECK_FALSE(f.stems.hasStemsAvailable());
        CHECK(f.stems.hasApproximateStems());
        CHECK(f.stems.stemSeparationInfo().isApproximate());
    }
    SECTION("no separation.json falls back to this run's result")
    {
        f.writeStems(stemsDir, nullptr);
        CHECK(f.stems.hasStemsAvailable()); // unknown provenance counts as real
        khensu::ipc::StemSeparationResult res;
        res.method_used = "hpss";
        f.sessions.state().lastSeparationResult = res;
        CHECK_FALSE(f.stems.hasStemsAvailable());
        CHECK(f.stems.hasApproximateStems());
    }
    SECTION("a missing stem means no stems")
    {
        f.writeStems(stemsDir, "demucs");
        REQUIRE(stemsDir.getChildFile("bass.wav").deleteFile());
        CHECK_FALSE(f.stems.stemFilesExist());
        CHECK_FALSE(f.stems.hasStemsAvailable());
        CHECK_FALSE(f.stems.getStemFile("bass").existsAsFile());
    }
}

TEST_CASE("StemService forgets the session's separation on reset", "[split][stems]")
{
    Fixture f;
    f.openSession("song.wav");
    f.sessions.state().separatingStems = true;
    f.sessions.state().vocalAnalysis = khensu::AnalysisResult{};
    f.sessions.state().lastSeparationResult = khensu::ipc::StemSeparationResult{};
    f.sessions.state().soloedStemName = "vocals";
    REQUIRE(f.stems.isSeparatingStems());
    REQUIRE(f.stems.hasVocalAnalysis());
    REQUIRE(f.stems.getLastSeparationResult().has_value());

    f.stems.resetForSession();
    CHECK_FALSE(f.stems.isSeparatingStems());
    CHECK_FALSE(f.stems.hasVocalAnalysis());
    CHECK_FALSE(f.stems.getLastSeparationResult().has_value());
    CHECK(f.sessions.state().soloedStemName.isEmpty());

    f.sessions.state().vocalAnalysis = khensu::AnalysisResult{};
    f.stems.clearVocalAnalysis();
    CHECK_FALSE(f.stems.hasVocalAnalysis());

    // A separation for the open session marks it in flight and queues the
    // long-running poll loop.
    bool called = false;
    f.stems.runStemSeparation("demucs", [&](bool, juce::String) { called = true; });
    CHECK(f.stems.isSeparatingStems());
    CHECK(f.work.jobs == 1);
    CHECK_FALSE(called);
    f.stems.resetForSession(); // session change: no separation running
    CHECK_FALSE(f.stems.isSeparatingStems());

    f.stems.setLastAutoStemMessage("ok: done");
    CHECK(f.stems.lastAutoStemMessage() == "ok: done");
}

namespace {
// Runs each job inline so the separation's poll loop executes in the test.
struct InlineWork final : khensu::IBackgroundWork {
    void runInBackground(std::function<void()> job) override { job(); }
    void runLongInBackground(std::function<void()> job) override { job(); }
    uint64_t sessionEpoch() const noexcept override { return 0; }
    bool isShuttingDown() const noexcept override { return false; }
};
} // namespace

// A separation that never started (here: the identity gate is closed, so the
// bridge refuses before any network call) reports why. The poller's reason
// used to be replaced by a generic "Stem separation did not complete".
TEST_CASE("StemService reports why a separation could not start", "[split][stems]")
{
    using khensu::ipc::HttpServiceBridge;
    HttpServiceBridge::setServiceIdentityRequired(true);
    HttpServiceBridge::setServiceIdentityVerified(false);

    const auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                         .getChildFile("khensu-stem-start-" + juce::Uuid().toString());
    REQUIRE(dir.createDirectory());
    InlineWork work;
    SessionService sessions;
    StemService stems{work, sessions};
    const auto source = dir.getChildFile("song.wav");
    REQUIRE(source.replaceWithText("not audio"));
    khensu::Session session;
    session.id = "session-1";
    khensu::AudioFileRef ref;
    ref.id = "f";
    ref.path = source.getFullPathName().toStdString();
    session.files.push_back(ref);
    sessions.adopt(std::move(session));
    stems.setIdentityWaitMs(0); // the gate stays closed: fail at once

    bool called = false, ok = true;
    juce::String message;
    stems.runStemSeparation("demucs", [&](bool success, juce::String m) {
        called = true;
        ok = success;
        message = m;
    });
    for (int i = 0; i < 50 && !called; ++i)
        juce::MessageManager::getInstance()->runDispatchLoopUntil(20);

    HttpServiceBridge::setServiceIdentityRequired(false);
    dir.deleteRecursively();
    REQUIRE(called);
    CHECK_FALSE(ok);
    CHECK(message.contains("could not be started"));
    CHECK(message.contains("identity not verified"));
    CHECK_FALSE(message.contains("did not complete"));
    CHECK_FALSE(stems.isSeparatingStems());
}

// The gate is closed when the separation is asked for (a cold engine not yet
// proven, or a busy one that missed its health probes) and opens a moment
// later: the separation waits for it instead of failing with "service
// identity not verified" (the app-loop failure of 2026-11-23).
TEST_CASE("StemService waits for the engine's identity before starting", "[split][stems]")
{
    using khensu::ipc::HttpServiceBridge;
    HttpServiceBridge::setServiceIdentityRequired(true);
    HttpServiceBridge::setServiceIdentityVerified(false);

    const auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                         .getChildFile("khensu-stem-wait-" + juce::Uuid().toString());
    REQUIRE(dir.createDirectory());
    InlineWork work;
    SessionService sessions;
    StemService stems{work, sessions};
    stems.setIdentityWaitMs(5000);
    const auto source = dir.getChildFile("song.wav");
    REQUIRE(source.replaceWithText("not audio"));
    khensu::Session session;
    session.id = "session-1";
    khensu::AudioFileRef ref;
    ref.id = "f";
    ref.path = source.getFullPathName().toStdString();
    session.files.push_back(ref);
    sessions.adopt(std::move(session));

    // The health monitor proves the engine 300 ms in; the start then gets
    // past the gate (and fails, or is refused, at the service - not here).
    std::thread prover([] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        HttpServiceBridge::setServiceIdentityVerified(true);
    });

    bool called = false;
    juce::String message;
    const auto t0 = juce::Time::getMillisecondCounter();
    stems.runStemSeparation("demucs", [&](bool, juce::String m) {
        called = true;
        message = m;
    });
    for (int i = 0; i < 1500 && !called; ++i)
        juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
    const auto waited = juce::Time::getMillisecondCounter() - t0;
    prover.join();

    HttpServiceBridge::setServiceIdentityVerified(false);
    HttpServiceBridge::setServiceIdentityRequired(false);
    dir.deleteRecursively();
    REQUIRE(called);
    CHECK_FALSE(message.contains("identity not verified"));
    CHECK(waited >= 250u);
}
