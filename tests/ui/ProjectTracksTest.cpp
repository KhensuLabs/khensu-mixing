// tests/ui/ProjectTracksTest.cpp
// FEAT-6: a project's tracks with their current masters, as the readiness
// board and Album Assembly read them.
#include <catch2/catch_test_macros.hpp>
#include "session/ProjectTracks.h"
#include "session/WorkspaceRecoveryState.h"

using namespace khensu;

namespace {
struct TempDir {
    juce::File dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                         .getChildFile("khensu_proj_" + juce::String(juce::Random::getSystemRandom().nextInt64()));
    TempDir() { dir.createDirectory(); }
    ~TempDir() { dir.deleteRecursively(); }
};

Session sessionWithFile(SessionManager& db, const std::string& name, const juce::File& file)
{
    auto s = db.createSession(name).value();
    AudioFileRef f;
    f.path = file.getFullPathName().toStdString();
    s.files = {f};
    REQUIRE(db.saveSession(s).is_ok());
    return s;
}
} // namespace

TEST_CASE("a track's current master is its accepted render, else its mix (FEAT-6)", "[project]")
{
    TempDir tmp;
    SessionManager db(":memory:");
    const auto mix = tmp.dir.getChildFile("song.wav");
    REQUIRE(mix.replaceWithText("x"));
    const auto s = sessionWithFile(db, "Song", mix);

    bool isRender = true;
    CHECK(project_tracks::currentMasterPath(db, s, &isRender) == mix.getFullPathName());
    CHECK_FALSE(isRender);

    const auto render = tmp.dir.getChildFile("song_master.wav");
    WorkspaceRecoveryState ws{render.getFullPathName(), {}, {}, {}};
    REQUIRE(db.setUserPreference("workspace_recovery_" + s.id, workspaceRecoveryStateToJson(ws).toStdString()).is_ok());
    // Saved but gone from disk: the mix.
    CHECK(project_tracks::currentMasterPath(db, s, &isRender) == mix.getFullPathName());
    REQUIRE(render.replaceWithText("x"));
    CHECK(project_tracks::currentMasterPath(db, s, &isRender) == render.getFullPathName());
    CHECK(isRender);
}

TEST_CASE("the board reads the active session's project in order (FEAT-6)", "[project]")
{
    TempDir tmp;
    SessionManager db(":memory:");
    const auto fa = tmp.dir.getChildFile("01 intro.wav"), fb = tmp.dir.getChildFile("02 single.wav");
    REQUIRE(fa.replaceWithText("x"));
    const auto a = sessionWithFile(db, "Intro", fa);
    const auto b = sessionWithFile(db, "Single", fb); // its file is missing
    const auto pid = db.createProject("EP").value().id;
    REQUIRE(db.addSessionToProject(pid, b.id).is_ok());
    REQUIRE(db.addSessionToProject(pid, a.id).is_ok());

    const auto snap = project_tracks::load(db, a.id);
    REQUIRE(snap.current.has_value());
    CHECK(snap.projects.size() == 1);
    REQUIRE(snap.tracks.size() == 2);
    CHECK(snap.tracks[0].name == "Single");
    CHECK(snap.tracks[0].masterMissing);
    CHECK(snap.tracks[1].masterPath == fa.getFullPathName());
    CHECK_FALSE(snap.tracks[1].summary.has_value()); // not analysed yet

    CHECK_FALSE(project_tracks::load(db, "not-in-a-project").current.has_value());
}

TEST_CASE("a drop's project is named after its folder (FEAT-6)", "[project]")
{
    AudioFileRef a, b, c;
    a.path = "D:/Albums/Night Drive/01.wav";
    b.path = "D:/Albums/Night Drive/02.wav";
    c.path = "E:/elsewhere/03.wav";
    CHECK(project_tracks::nameForFiles({a, b}) == "Night Drive");
    CHECK(project_tracks::nameForFiles({a, b, c}) == "3 tracks");
    CHECK(project_tracks::nameForFiles({}).isEmpty());
}

#include "support/FakeAppController.h"
#include "ui/ExportDelivery/ProjectBoardPanel.h"

TEST_CASE("the readiness board lists the open track's project (FEAT-6)", "[project][ui]")
{
    test::FakeAppController c;
    auto& db = c.sessionManager();
    TempDir tmp;
    const auto f1 = tmp.dir.getChildFile("one.wav"), f2 = tmp.dir.getChildFile("two.wav");
    REQUIRE(f1.replaceWithText("x"));
    REQUIRE(f2.replaceWithText("x"));
    const auto a = sessionWithFile(db, "Board One", f1);
    const auto b = sessionWithFile(db, "Board Two", f2);
    const auto pid = db.createProject("Board EP").value().id;
    REQUIRE(db.addSessionToProject(pid, a.id).is_ok());
    REQUIRE(db.addSessionToProject(pid, b.id).is_ok());
    c.session = b;

    ProjectBoardPanel board(c);
    board.setBounds(0, 0, 640, 320);
    juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
    REQUIRE(board.tracks().size() == 2);
    CHECK(board.tracks()[0].name == "Board One");
    CHECK(board.tracks()[1].masterPath == f2.getFullPathName());

    // Eyeball it: KHENSU_TEST_SNAPSHOT_DIR=<dir> writes the board as a PNG.
    if (const auto dir = juce::SystemStats::getEnvironmentVariable("KHENSU_TEST_SNAPSHOT_DIR", {}); dir.isNotEmpty()) {
        const auto image = board.createComponentSnapshot(board.getLocalBounds());
        juce::FileOutputStream out(juce::File(dir).getChildFile("project-board.png"));
        if (out.openedOk()) juce::PNGImageFormat().writeImageToStream(image, out);
    }
    REQUIRE(db.deleteProject(pid).is_ok()); // the store is shared between tests
}
