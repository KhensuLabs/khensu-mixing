#include <catch2/catch_test_macros.hpp>
#include <khensu/feedback/FeedbackManager.h>
#include <khensu/feedback/IssueState.h>

#include <SQLiteCpp/SQLiteCpp.h>
#include <juce_core/juce_core.h>

using namespace khensu;

namespace {
// Writes a legacy feedback.db (pre-unification schema: numeric user_preferences)
// with one row per table, at `dir/feedback.db`.
void seedLegacyFeedbackDb(const juce::File& dir)
{
    const auto path = dir.getChildFile("feedback.db").getFullPathName().toStdString();
    SQLite::Database db(path, SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
    db.exec("CREATE TABLE recommendation_feedback (id TEXT PRIMARY KEY, recommendation_id TEXT NOT NULL UNIQUE, "
            "acted_on INTEGER, feedback_note TEXT, created_at TEXT, updated_at TEXT)");
    db.exec("CREATE TABLE user_preferences (pref_key TEXT PRIMARY KEY, pref_value REAL NOT NULL)");
    db.exec("CREATE TABLE parameter_overrides (id TEXT PRIMARY KEY, recommendation_id TEXT NOT NULL, "
            "parameter_name TEXT NOT NULL, recommended_value REAL NOT NULL, actual_value REAL NOT NULL, created_at TEXT)");
    db.exec("INSERT INTO recommendation_feedback VALUES ('i1','recLegacy',1,'legacy note','t0','t0')");
    db.exec("INSERT INTO user_preferences VALUES ('lufs_offset', 1.5)");
    db.exec("INSERT INTO parameter_overrides VALUES ('o1','recLegacy','gain_db',2.0,3.0,'t0')");
}
} // namespace

TEST_CASE("FeedbackManager records and retrieves acted_on", "[feedback]")
{
    // Use an in-memory DB for tests
    FeedbackManager mgr(":memory:");

    auto result = mgr.recordActedOn("lufs_too_loud_test", true, "Fixed by -2 dB");
    REQUIRE(result.is_ok());

    auto query = mgr.getActedOn("lufs_too_loud_test");
    REQUIRE(query.is_ok());
    CHECK(query.value() == true);
}

TEST_CASE("FeedbackManager returns error for unknown recommendation", "[feedback]")
{
    FeedbackManager mgr(":memory:");
    auto query = mgr.getActedOn("nonexistent_rec_id");
    CHECK_FALSE(query.is_ok());
}

TEST_CASE("FeedbackManager updates existing entry", "[feedback]")
{
    FeedbackManager mgr(":memory:");
    mgr.recordActedOn("lra_test", false, "");
    mgr.recordActedOn("lra_test", true, "Fixed on second pass");
    auto q = mgr.getActedOn("lra_test");
    REQUIRE(q.is_ok());
    CHECK(q.value() == true);
}

// deleteAll() (audit DATA-02): the feedback.db half of "Delete all Khensu
// data" - erases recommendation_feedback, user_preferences (learning
// weights), and parameter_overrides.

TEST_CASE("FeedbackManager::deleteAll erases recorded acted-on state", "[feedback]")
{
    FeedbackManager mgr(":memory:");
    mgr.recordActedOn("rec1", true, "note");
    REQUIRE(mgr.getActedOn("rec1").is_ok());

    REQUIRE(mgr.deleteAll().is_ok());

    CHECK_FALSE(mgr.getActedOn("rec1").is_ok());
}

TEST_CASE("FeedbackManager::deleteAll erases preferences and overrides too", "[feedback]")
{
    FeedbackManager mgr(":memory:");
    mgr.setPreference("lufs_offset", 0.5f);
    mgr.recordOverride("rec1", "gain_db", 2.0f, 3.0f);

    REQUIRE(mgr.deleteAll().is_ok());

    // getPreference falls back to its default when the key is gone.
    auto pref = mgr.getPreference("lufs_offset", -99.0f);
    REQUIRE(pref.is_ok());
    CHECK(pref.value() == -99.0f);
    auto overrides = mgr.listOverrides("rec1");
    REQUIRE(overrides.is_ok());
    CHECK(overrides.value().empty());
}

TEST_CASE("FeedbackManager::deleteAll on an already-empty store still succeeds", "[feedback]")
{
    FeedbackManager mgr(":memory:");
    CHECK(mgr.deleteAll().is_ok());
}

// DB unification Phase 3 (ARCH-03): opening khensu.db one-time-imports the
// legacy feedback.db (backup-first, marker-guarded, idempotent), mapping its
// numeric user_preferences into recommendation_prefs.
TEST_CASE("FeedbackManager imports a legacy feedback.db into khensu.db once", "[feedback][migrations]")
{
    juce::File tmp = juce::File::getSpecialLocation(juce::File::tempDirectory)
                         .getChildFile("khensu_fb_import_" + juce::String(juce::Random::getSystemRandom().nextInt()));
    tmp.createDirectory();
    const auto khensu = tmp.getChildFile("khensu.db").getFullPathName().toStdString();
    seedLegacyFeedbackDb(tmp);

    {
        FeedbackManager mgr(khensu);   // construction runs the import
        auto acted = mgr.getActedOn("recLegacy");
        REQUIRE(acted.is_ok());
        CHECK(acted.value() == true);                                  // recommendation_feedback imported
        CHECK(mgr.getPreference("lufs_offset", -99.0f).value() == 1.5f); // user_preferences -> recommendation_prefs
        auto ov = mgr.listOverrides("recLegacy");
        REQUIRE(ov.is_ok());
        CHECK(ov.value().size() == 1);                                 // parameter_overrides imported
    }

    // Backed up, retired (not deleted), and the original is gone.
    CHECK(tmp.getChildFile("feedback.db.bak").existsAsFile());
    CHECK(tmp.getChildFile("feedback.db.migrated").existsAsFile());
    CHECK_FALSE(tmp.getChildFile("feedback.db").existsAsFile());

    // Reopening is a no-op (marker set) and the data is still there.
    {
        FeedbackManager mgr(khensu);
        CHECK(mgr.getActedOn("recLegacy").value() == true);
    }

    tmp.deleteRecursively();
}

// AUDIT-059: a legacy feedback.db whose last commits still live only in
// feedback.db-wal (the app crashed / was killed before a checkpoint) must
// import those rows too, and the sidecars must move with the retired file.
TEST_CASE("FeedbackManager imports rows that only exist in the legacy WAL", "[feedback][migrations]")
{
    juce::File tmp = juce::File::getSpecialLocation(juce::File::tempDirectory)
                         .getChildFile("khensu_fb_wal_" + juce::String(juce::Random::getSystemRandom().nextInt()));
    tmp.createDirectory();
    const juce::File staging = tmp.getChildFile("staging");
    staging.createDirectory();
    seedLegacyFeedbackDb(staging);
    {
        // Commit a row into the WAL only, then snapshot db + wal as a crashed
        // process would have left them (SQLite shares the files for reading).
        SQLite::Database db(staging.getChildFile("feedback.db").getFullPathName().toStdString(),
                            SQLite::OPEN_READWRITE);
        db.exec("PRAGMA journal_mode=WAL");
        db.exec("PRAGMA wal_autocheckpoint=0");
        db.exec("INSERT INTO recommendation_feedback VALUES ('i2','recWalOnly',1,'wal note','t1','t1')");
        REQUIRE(staging.getChildFile("feedback.db-wal").existsAsFile());
        REQUIRE(staging.getChildFile("feedback.db").copyFileTo(tmp.getChildFile("feedback.db")));
        REQUIRE(staging.getChildFile("feedback.db-wal").copyFileTo(tmp.getChildFile("feedback.db-wal")));
    }

    const auto khensu = tmp.getChildFile("khensu.db").getFullPathName().toStdString();
    {
        FeedbackManager mgr(khensu);
        auto walRow = mgr.getActedOn("recWalOnly");
        REQUIRE(walRow.is_ok());
        CHECK(walRow.value() == true);
        CHECK(mgr.getActedOn("recLegacy").value() == true);
    }
    CHECK(tmp.getChildFile("feedback.db.bak").existsAsFile());
    CHECK(tmp.getChildFile("feedback.db.migrated").existsAsFile());
    CHECK_FALSE(tmp.getChildFile("feedback.db").existsAsFile());
    CHECK_FALSE(tmp.getChildFile("feedback.db-wal").existsAsFile()); // checkpointed, not left behind
    // The backup alone is complete: it holds the WAL-only row.
    {
        SQLite::Database bak(tmp.getChildFile("feedback.db.bak").getFullPathName().toStdString(),
                             SQLite::OPEN_READONLY);
        SQLite::Statement q(bak, "SELECT COUNT(*) FROM recommendation_feedback WHERE recommendation_id='recWalOnly'");
        REQUIRE(q.executeStep());
        CHECK(q.getColumn(0).getInt() == 1);
    }
    tmp.deleteRecursively();
}

TEST_CASE("FeedbackManager defers the legacy import when the backup cannot be written", "[feedback][migrations]")
{
    juce::File tmp = juce::File::getSpecialLocation(juce::File::tempDirectory)
                         .getChildFile("khensu_fb_nobak_" + juce::String(juce::Random::getSystemRandom().nextInt()));
    tmp.createDirectory();
    seedLegacyFeedbackDb(tmp);
    // A DIRECTORY where the backup file should go makes copyFileTo fail.
    REQUIRE(tmp.getChildFile("feedback.db.bak").createDirectory());
    REQUIRE(tmp.getChildFile("feedback.db.bak").getChildFile("blocker").create());

    const auto khensu = tmp.getChildFile("khensu.db").getFullPathName().toStdString();
    {
        FeedbackManager mgr(khensu);
        CHECK_FALSE(mgr.getActedOn("recLegacy").is_ok()); // nothing imported
    }
    CHECK(tmp.getChildFile("feedback.db").existsAsFile()); // not retired either
    CHECK_FALSE(tmp.getChildFile("feedback.db.migrated").existsAsFile());
    tmp.deleteRecursively();
}

TEST_CASE("issue_state reads the issue queue's stored rows", "[feedback][con1]")
{
    using issue_state::State;
    CHECK(issue_state::of({true, ""}) == State::Done);
    CHECK(issue_state::of({true, "dismissed"}) == State::Done); // acted on wins
    CHECK(issue_state::of({false, "dismissed"}) == State::Dismissed);
    CHECK(issue_state::of({false, "dismissed: Intentional"}) == State::Dismissed);
    CHECK(issue_state::of({false, "reopened"}) == State::Open);
    CHECK(issue_state::of({false, ""}) == State::Open);
    CHECK(issue_state::dismissNote("") == "dismissed");
    CHECK(issue_state::dismissNote("Too bright") == "dismissed: Too bright");

    // The plugin's single toggle: open -> done, done / dismissed -> open.
    CHECK(issue_state::toggled(State::Open) == State::Done);
    CHECK(issue_state::toggled(State::Done) == State::Open);
    CHECK(issue_state::toggled(State::Dismissed) == State::Open);
    for (const auto s : {State::Open, State::Done, State::Dismissed})
        CHECK(issue_state::of(issue_state::rowFor(s)) == s);
}

TEST_CASE("A plugin change and a desktop dismissal read the same through the store", "[feedback][con1]")
{
    using issue_state::State;
    FeedbackManager mgr(":memory:");
    const auto store = [&mgr](const std::string& id, State s) {
        const auto row = issue_state::rowFor(s);
        REQUIRE(mgr.recordActedOn(id, row.acted_on, row.note).is_ok());
    };
    const auto read = [&mgr](const std::string& id) {
        const auto rows = mgr.getFeedbackBatch({id});
        return rows.count(id) ? issue_state::of(rows.at(id)) : State::Open;
    };

    // The desktop dismissed it: the plugin reads dismissed, not open.
    REQUIRE(mgr.recordActedOn("harsh_f1", false, issue_state::dismissNote("Intentional")).is_ok());
    CHECK(read("harsh_f1") == State::Dismissed);
    // Clicking it in the plugin reopens it for the desktop too.
    store("harsh_f1", issue_state::toggled(read("harsh_f1")));
    CHECK(read("harsh_f1") == State::Open);
    // Marked done in the plugin reads done on the desktop.
    store("harsh_f1", issue_state::toggled(read("harsh_f1")));
    CHECK(read("harsh_f1") == State::Done);
    CHECK(read("never_touched_f1") == State::Open);
}
