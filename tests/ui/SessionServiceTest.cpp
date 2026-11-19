// tests/ui/SessionServiceTest.cpp
// Pins SessionService (ENG-1 step 3b, extracted from AppController): store
// open with degraded fallback, create / load outcomes, relink, recent lists
// and the session-reset fan-out - all without an AppController.
#include <catch2/catch_test_macros.hpp>
#include "session/SessionService.h"

using khensu::SessionService;

namespace {
struct TempDb {
    juce::File dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                         .getChildFile("khensu-session-service-" + juce::Uuid().toString());
    TempDb() { dir.createDirectory(); }
    ~TempDb() { dir.deleteRecursively(); }
    std::string path() const { return dir.getChildFile("khensu.db").getFullPathName().toStdString(); }
};

khensu::AudioFileRef fileRef(const char* path)
{
    khensu::AudioFileRef ref;
    ref.id = juce::Uuid().toString().toStdString();
    ref.path = path;
    ref.format = "wav";
    return ref;
}

struct CountingScoped final : khensu::SessionScoped {
    int resets = 0;
    void resetForSession() override { ++resets; }
};
} // namespace

TEST_CASE("SessionService opens a real store and round-trips a session", "[split][session]")
{
    TempDb db;
    auto store = SessionService::openStore(db.path(), 1);
    REQUIRE(store.manager != nullptr);
    CHECK_FALSE(store.degraded);

    SessionService sessions;
    sessions.attachStore(store.manager);
    CHECK_FALSE(sessions.hasSession());
    CHECK(sessions.activeSourcePath().isEmpty());

    auto created = sessions.create("mix", {fileRef("C:/audio/song.wav")}, khensu::CreativeTarget{});
    REQUIRE(created.has_value());

    auto loaded = sessions.load(created->id);
    REQUIRE(loaded.status == SessionService::LoadStatus::Loaded);
    REQUIRE(loaded.session.has_value());
    REQUIRE(loaded.session->files.size() == 1);
    CHECK(loaded.session->files[0].path == "C:/audio/song.wav");

    CHECK(sessions.load("no-such-session").status == SessionService::LoadStatus::NotFound);

    const auto recent = sessions.listRecent(10);
    CHECK_FALSE(sessions.recentLoadFailed());
    REQUIRE(recent.size() == 1);
    CHECK(recent[0].id == created->id);
}

TEST_CASE("SessionService relinks only an open session", "[split][session]")
{
    TempDb db;
    SessionService sessions;
    sessions.attachStore(SessionService::openStore(db.path(), 1).manager);

    CHECK_FALSE(sessions.relinkSource("D:/moved.wav").has_value()); // nothing open

    auto created = sessions.create("mix", {fileRef("C:/audio/song.wav")}, khensu::CreativeTarget{});
    REQUIRE(created.has_value());
    sessions.adopt(*created);
    CHECK(sessions.activeSessionId() == juce::String(created->id));
    CHECK(sessions.activeSourcePath() == "C:/audio/song.wav");

    auto relinked = sessions.relinkSource("D:/moved.wav");
    REQUIRE(relinked.has_value());
    CHECK(relinked->files[0].path == "D:/moved.wav");
    CHECK(sessions.activeSourcePath() == "D:/moved.wav");

    // The returned copy is what gets persisted (off the message thread in the app).
    REQUIRE(sessions.manager().saveSession(*relinked).is_ok());
    CHECK(sessions.load(created->id).session->files[0].path == "D:/moved.wav");
}

TEST_CASE("SessionService falls back to an in-memory store when the DB can't open", "[split][session]")
{
    TempDb db;
    // A directory where the database file should be: SQLite can't open it.
    const auto blocked = db.dir.getChildFile("blocked.db");
    REQUIRE(blocked.createDirectory());

    auto store = SessionService::openStore(blocked.getFullPathName().toStdString(), 1);
    REQUIRE(store.manager != nullptr);
    CHECK(store.degraded);
    CHECK(store.failure.isNotEmpty());

    SessionService sessions;
    sessions.attachStore(store.manager);
    CHECK(sessions.create("still works", {}, khensu::CreativeTarget{}).has_value());
}

TEST_CASE("SessionService fans session resets out to registered components", "[split][session]")
{
    SessionService sessions;
    CountingScoped a, b;
    sessions.addScoped(&a);
    sessions.addScoped(&b);
    sessions.addScoped(&a); // duplicate registration is ignored
    sessions.resetAllScoped();
    CHECK(a.resets == 1);
    CHECK(b.resets == 1);
    sessions.removeScoped(&a);
    sessions.resetAllScoped();
    CHECK(a.resets == 1);
    CHECK(b.resets == 2);
}
