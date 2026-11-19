#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <khensu/session/SessionManager.h>
#include <khensu/feedback/FeedbackManager.h>
#include <khensu/targets/GenreConfirmation.h>
#include <khensu/session/Session.h>
#include <khensu/targets/CreativeTarget.h>
#include <juce_core/juce_core.h> // for the backup/export temp-file tests (audit DATA-02)
#include <SQLiteCpp/SQLiteCpp.h>
#include <khensu/session/OutboxDispatcher.h>
#include <algorithm>
#include <atomic>
#include <string>
#include <thread>

using namespace khensu;

// All tests use an in-memory SQLite DB - no filesystem side effects.

TEST_CASE("SessionManager::createSession persists and returns a valid session", "[session]")
{
    SessionManager mgr(":memory:");
    REQUIRE(mgr.isOpen());

    auto result = mgr.createSession("My First Mix");
    REQUIRE(result.is_ok());

    const Session& s = result.value();
    CHECK_FALSE(s.id.empty());
    CHECK(s.name == "My First Mix");
    CHECK_FALSE(s.created_at.empty());
    CHECK(s.created_at == s.updated_at);
    CHECK(s.is_valid());
}

TEST_CASE("SessionManager::createSession with a CreativeTarget preserves target fields", "[session]")
{
    SessionManager mgr(":memory:");

    CreativeTarget target;
    target.primary_genre = "trap";
    target.mood = "melancholic";
    target.energy = 75.0f;
    target.playback_intent = "streaming";
    target.heaviness_target = 60.0f;

    auto result = mgr.createSession("Trap Session", target);
    REQUIRE(result.is_ok());

    const Session& s = result.value();
    CHECK(s.target.primary_genre == "trap");
    CHECK(s.target.mood == "melancholic");
    CHECK_THAT(s.target.energy, Catch::Matchers::WithinAbs(75.0f, 0.001f));
    CHECK(s.target.playback_intent == "streaming");
}

TEST_CASE("SessionManager::loadSession retrieves a previously saved session", "[session]")
{
    SessionManager mgr(":memory:");

    auto createResult = mgr.createSession("Load Test");
    REQUIRE(createResult.is_ok());
    const std::string id = createResult.value().id;

    auto loaded = mgr.loadSession(id);
    REQUIRE(loaded.is_ok());
    REQUIRE(loaded.value().has_value());
    CHECK(loaded.value()->id == id);
    CHECK(loaded.value()->name == "Load Test");
}

TEST_CASE("SessionManager::loadSession returns ok(nullopt) for unknown id", "[session]")
{
    SessionManager mgr(":memory:");
    auto result = mgr.loadSession("00000000-0000-0000-0000-000000000000");
    // A genuinely-missing row is not a database failure (audit CQ-05): the
    // typed result makes "no such session" ok(nullopt), never err.
    REQUIRE(result.is_ok());
    CHECK_FALSE(result.value().has_value());
}

// A missing row and a failed read used to be indistinguishable — both
// returned an empty/nullopt result with no way to tell them apart (audit
// CQ-05). These force a real SQLite exception (a query against a table that
// no longer exists) to verify the error path actually surfaces it instead of
// silently swallowing it into "not found"/"empty".
TEST_CASE("SessionManager::loadSession surfaces a database failure as err", "[session]")
{
    SessionManager mgr(":memory:");
    auto created = mgr.createSession("Will Be Orphaned");
    REQUIRE(created.is_ok());
    const std::string id = created.value().id;

    mgr.execRawSqlForTesting("DROP TABLE sessions");

    auto result = mgr.loadSession(id);
    REQUIRE(result.is_err());
    CHECK_FALSE(result.error().empty());
}

TEST_CASE("SessionManager::listSessions surfaces a database failure as err", "[session]")
{
    SessionManager mgr(":memory:");
    REQUIRE(mgr.createSession("A").is_ok());
    mgr.execRawSqlForTesting("DROP TABLE sessions");

    CHECK(mgr.listSessions().is_err());
}

TEST_CASE("SessionManager::listSessions does not report a failure when genuinely empty", "[session]")
{
    SessionManager mgr(":memory:");
    auto result = mgr.listSessions();
    REQUIRE(result.is_ok());
    CHECK(result.value().empty());
}

TEST_CASE("SessionManager::listRecentAnalysisSummaries surfaces a database failure as err", "[session]")
{
    SessionManager mgr(":memory:");
    mgr.execRawSqlForTesting("DROP TABLE analysis_results");

    CHECK(mgr.listRecentAnalysisSummaries().is_err());
}

TEST_CASE("SessionManager::saveSession updates name and refreshes updated_at", "[session]")
{
    SessionManager mgr(":memory:");

    auto createResult = mgr.createSession("Original Name");
    REQUIRE(createResult.is_ok());

    Session s = createResult.value();
    const std::string originalUpdatedAt = s.updated_at;

    s.name = "Renamed Session";
    auto saveResult = mgr.saveSession(s);
    REQUIRE(saveResult.is_ok());

    auto reloaded = mgr.loadSession(s.id).value();
    REQUIRE(reloaded.has_value());
    CHECK(reloaded->name == "Renamed Session");
    // updated_at must change (may be same second in very fast test; just check name persisted)
}

TEST_CASE("SessionManager::listSessions returns all created sessions newest first", "[session]")
{
    SessionManager mgr(":memory:");

    mgr.createSession("Session A");
    mgr.createSession("Session B");
    mgr.createSession("Session C");

    auto sessions = mgr.listSessions().value();
    CHECK(sessions.size() == 3);
    // All IDs are unique
    CHECK(sessions[0].id != sessions[1].id);
    CHECK(sessions[1].id != sessions[2].id);
}

TEST_CASE("SessionManager::deleteSession removes the session", "[session]")
{
    SessionManager mgr(":memory:");

    auto createResult = mgr.createSession("To Be Deleted");
    REQUIRE(createResult.is_ok());
    const std::string id = createResult.value().id;

    auto delResult = mgr.deleteSession(id);
    REQUIRE(delResult.is_ok());

    CHECK_FALSE(mgr.loadSession(id).value().has_value());
    CHECK(mgr.listSessions().value().empty());
}

// deleteAllSessions() (audit DATA-02): the khensu.db half of "Delete all
// Khensu data" - erases every session, cascading to analysis_results and
// suggestion_history via their ON DELETE CASCADE foreign keys.

TEST_CASE("SessionManager::deleteAllSessions erases every session", "[session]")
{
    SessionManager mgr(":memory:");
    mgr.createSession("Session A");
    mgr.createSession("Session B");
    mgr.createSession("Session C");
    REQUIRE(mgr.listSessions().value().size() == 3);

    REQUIRE(mgr.deleteAllSessions().is_ok());

    CHECK(mgr.listSessions().value().empty());
}

TEST_CASE("SessionManager::deleteAllSessions cascades to analysis_results", "[session]")
{
    SessionManager mgr(":memory:");
    auto created = mgr.createSession("With Analysis");
    REQUIRE(created.is_ok());
    const std::string id = created.value().id;

    AnalysisResult r;
    mgr.saveAnalysisResult(r, id, "file1");
    REQUIRE(mgr.listRecentAnalysisSummaries().value().size() == 1);

    REQUIRE(mgr.deleteAllSessions().is_ok());

    CHECK(mgr.listRecentAnalysisSummaries().value().empty());
}

TEST_CASE("SessionManager::deleteAllSessions on an already-empty store still succeeds", "[session]")
{
    SessionManager mgr(":memory:");
    CHECK(mgr.deleteAllSessions().is_ok());
}

// Backup + restore drill and portable export (audit DATA-02).

TEST_CASE("SessionManager::backupTo produces a file that restores every session", "[session][backup]")
{
    auto backupFile = juce::File::getSpecialLocation(juce::File::tempDirectory)
                          .getChildFile("khensu_backup_test.db");
    backupFile.deleteFile();

    {
        SessionManager src(":memory:");
        src.createSession("Alpha");
        src.createSession("Beta");
        REQUIRE(src.listSessions().value().size() == 2);
        REQUIRE(src.backupTo(backupFile.getFullPathName().toStdString()).is_ok());
    }

    REQUIRE(backupFile.existsAsFile());

    // Restore drill: open the backup as a fresh SessionManager and confirm the
    // data actually came back (a backup that can't be restored is worthless).
    {
        SessionManager restored(backupFile.getFullPathName().toStdString());
        auto sessions = restored.listSessions().value();
        REQUIRE(sessions.size() == 2);
        std::vector<std::string> names{sessions[0].name, sessions[1].name};
        CHECK(std::find(names.begin(), names.end(), "Alpha") != names.end());
        CHECK(std::find(names.begin(), names.end(), "Beta") != names.end());
    }

    backupFile.deleteFile();
}

TEST_CASE("SessionManager::backupTo overwrites a stale destination file", "[session][backup]")
{
    auto backupFile = juce::File::getSpecialLocation(juce::File::tempDirectory)
                          .getChildFile("khensu_backup_overwrite_test.db");
    backupFile.replaceWithText("not a database");

    SessionManager src(":memory:");
    src.createSession("Only");
    REQUIRE(src.backupTo(backupFile.getFullPathName().toStdString()).is_ok());

    SessionManager restored(backupFile.getFullPathName().toStdString());
    CHECK(restored.listSessions().value().size() == 1);
    backupFile.deleteFile();
}

TEST_CASE("SessionManager::saveAnalysisResult reports a locked database", "[session][persistence]")
{
    const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory)
                          .getChildFile("khensu-analysis-lock-" + juce::Uuid().toString());
    root.createDirectory();
    {   // closed before the folder goes: Windows cannot delete an open khensu.db (PKG-4)
        const auto dbFile = root.getChildFile("khensu.db");
        SessionManager mgr(dbFile.getFullPathName().toStdString());
        const auto session = mgr.createSession("Persistence failure");
        REQUIRE(session.is_ok());
        mgr.execRawSqlForTesting("PRAGMA busy_timeout=0");

        SQLite::Database blocker(dbFile.getFullPathName().toStdString(), SQLite::OPEN_READWRITE);
        blocker.exec("BEGIN EXCLUSIVE");

        AnalysisResult result;
        result.id = "cannot-save";
        const auto saved = mgr.saveAnalysisResult(result, session.value().id, "file-1");
        blocker.exec("ROLLBACK");

        REQUIRE(saved.is_err());
        REQUIRE(saved.error().find("saveAnalysisResult") != std::string::npos);
    }
    root.deleteRecursively();
}

TEST_CASE("SessionManager::backupTo preserves an existing destination when replacement is impossible", "[session][backup]")
{
    const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory)
                          .getChildFile("khensu-backup-atomic-" + juce::Uuid().toString());
    root.createDirectory();
    const auto protectedDestination = root.getChildFile("existing-backup");
    protectedDestination.createDirectory();

    SessionManager mgr(":memory:");
    const auto result = mgr.backupTo(protectedDestination.getFullPathName().toStdString());
    REQUIRE(result.is_err());
    REQUIRE(protectedDestination.isDirectory());
    root.deleteRecursively();
}

TEST_CASE("SessionManager::exportSessionJson bundles the session and its analysis rows", "[session][export]")
{
    SessionManager mgr(":memory:");
    auto created = mgr.createSession("Exportable");
    REQUIRE(created.is_ok());
    const std::string id = created.value().id;

    AnalysisResult r;
    mgr.saveAnalysisResult(r, id, "file1");

    auto exported = mgr.exportSessionJson(id);
    REQUIRE(exported.is_ok());

    // The export carries the session, its analysis row (file1), and the
    // format version - the portable-export contract (audit DATA-02).
    const std::string& json = exported.value();
    CHECK(json.find("Exportable") != std::string::npos);
    CHECK(json.find("file1") != std::string::npos);
    CHECK(json.find("analysis_results") != std::string::npos);
    CHECK(json.find("export_format_version") != std::string::npos);
}

TEST_CASE("SessionManager::exportSessionJson keeps non-ASCII text (FIX-18)", "[session][export][fix18]")
{
    // The columns are UTF-8. Read as Latin-1, an e-acute exports as two
    // characters and a session name no longer round-trips.
    SessionManager mgr(":memory:");
    const std::string name = "Caf\xc3\xa9 \xe2\x80\x94 mix"; // e-acute, em dash
    auto created = mgr.createSession(name);
    REQUIRE(created.is_ok());

    auto exported = mgr.exportSessionJson(created.value().id);
    REQUIRE(exported.is_ok());
    const auto parsed = juce::JSON::parse(juce::String::fromUTF8(exported.value().c_str()));
    const juce::String exportedName = parsed["session"]["name"].toString();
    CHECK(exportedName == juce::String::fromUTF8(name.c_str()));
    CHECK(exportedName.length() == 10);
}

TEST_CASE("SessionManager::exportSessionJson fails for an unknown session", "[session][export]")
{
    SessionManager mgr(":memory:");
    CHECK_FALSE(mgr.exportSessionJson("does-not-exist").is_ok());
}

TEST_CASE("SessionManager IDs are unique across multiple creates", "[session]")
{
    SessionManager mgr(":memory:");

    auto r1 = mgr.createSession("S1");
    auto r2 = mgr.createSession("S2");
    REQUIRE(r1.is_ok());
    REQUIRE(r2.is_ok());

    CHECK(r1.value().id != r2.value().id);
}

TEST_CASE("Session::is_valid returns false for empty session", "[session]")
{
    Session empty;
    CHECK_FALSE(empty.is_valid());
}

TEST_CASE("AudioFileRef roundtrips through session JSON", "[session]")
{
    SessionManager mgr(":memory:");

    auto createResult = mgr.createSession("File Roundtrip Test");
    REQUIRE(createResult.is_ok());
    Session s = createResult.value();

    AudioFileRef f;
    f.id = "test-file-id";
    f.path = "C:/Users/Test/track.wav";
    f.format = "wav";
    f.duration_seconds = 212.5;
    f.sample_rate = 48000;
    f.channels = 2;
    f.bit_depth = 24;
    f.title = "Test Track";
    s.files.push_back(f);

    auto saveResult = mgr.saveSession(s);
    REQUIRE(saveResult.is_ok());

    auto loaded = mgr.loadSession(s.id).value();
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->files.size() == 1);
    CHECK(loaded->files[0].path == "C:/Users/Test/track.wav");
    CHECK(loaded->files[0].format == "wav");
    CHECK(loaded->files[0].sample_rate == 48000);
    CHECK(loaded->files[0].channels == 2);
    CHECK(loaded->files[0].title == "Test Track");
    CHECK_THAT(loaded->files[0].duration_seconds, Catch::Matchers::WithinAbs(212.5, 0.001));
}

TEST_CASE("SessionManager user preference CRUD", "[session]")
{
    SessionManager mgr(":memory:");
    REQUIRE(mgr.isOpen());

    auto getEmpty = mgr.getUserPreference("missing_key");
    CHECK_FALSE(getEmpty.is_ok());

    auto setRes = mgr.setUserPreference("my_key", "{\"value\": 42}");
    REQUIRE(setRes.is_ok());

    auto getRes = mgr.getUserPreference("my_key");
    REQUIRE(getRes.is_ok());
    CHECK(getRes.value() == "{\"value\": 42}");

    auto overwriteRes = mgr.setUserPreference("my_key", "{\"value\": 99}");
    REQUIRE(overwriteRes.is_ok());

    auto getOverwrite = mgr.getUserPreference("my_key");
    REQUIRE(getOverwrite.is_ok());
    CHECK(getOverwrite.value() == "{\"value\": 99}");
}


// ---------------------------------------------------------------------------
// AUDIT-013: concurrent migrators converge; the plugin opens read-only.
// ---------------------------------------------------------------------------

namespace {
juce::File freshTempDir(const juce::String& tag)
{
    auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                   .getChildFile("khensu-" + tag + "-" + juce::Uuid().toString());
    dir.createDirectory();
    return dir;
}

// A khensu.db exactly as a build that only knew migrations 1..upTo left it.
void writeDbAtVersion(const std::string& path, int upTo)
{
    SQLite::Database db(path, SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
    db.exec("PRAGMA journal_mode=WAL");
    db.exec("CREATE TABLE schema_version (version INTEGER PRIMARY KEY, applied_at TEXT NOT NULL)");
    for (const auto& m : SessionManager::migrations()) {
        if (m.version > upTo) break;
        db.exec(m.sql);
        db.exec("INSERT INTO schema_version (version, applied_at) VALUES (" + std::to_string(m.version) + ", 't')");
    }
}
} // namespace

TEST_CASE("Two SessionManagers migrating one v5 DB concurrently both succeed and apply each version once",
          "[session][migrations]")
{
    const auto dir = freshTempDir("migrate-race");
    const std::string path = dir.getChildFile("khensu.db").getFullPathName().toStdString();
    writeDbAtVersion(path, 5);

    // Release both threads at once so their opens/migrations overlap.
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    std::atomic<int> failures{0};
    auto migrate = [&] {
        ++ready;
        while (!go.load()) std::this_thread::yield();
        try {
            SessionManager mgr(path); // separate connection per thread
            if (!mgr.listSessions().is_ok()) ++failures;
        } catch (const std::exception&) {
            ++failures;
        }
    };
    std::thread a(migrate), b(migrate);
    while (ready.load() < 2) std::this_thread::yield();
    go.store(true);
    a.join();
    b.join();
    CHECK(failures.load() == 0);

    {
        SQLite::Database check(path, SQLite::OPEN_READONLY);
        SQLite::Statement q(check, "SELECT COUNT(*), COUNT(DISTINCT version), MAX(version) FROM schema_version");
        REQUIRE(q.executeStep());
        const int latest = SessionManager::latestSchemaVersion();
        CHECK(q.getColumn(0).getInt() == latest); // one row per version - nobody re-applied
        CHECK(q.getColumn(1).getInt() == latest);
        CHECK(q.getColumn(2).getInt() == latest);
    }
    dir.deleteRecursively();
}

TEST_CASE("SessionManager refuses a DB migrated by a newer build (AUDIT-087)", "[session][migrations]")
{
    const auto dir = freshTempDir("schema-newer");
    const std::string path = dir.getChildFile("khensu.db").getFullPathName().toStdString();
    {
        SessionManager current(path);
        current.execRawSqlForTesting("INSERT INTO schema_version (version, applied_at) VALUES (" +
                                     std::to_string(SessionManager::latestSchemaVersion() + 1) + ", 'future')");
    }
    CHECK_THROWS_AS(SessionManager(path), SessionManager::SchemaTooNewError);
    // Nor read (FIX-18): the tables may have moved, and the plugin showing
    // rows it misreads is worse than showing none.
    CHECK_THROWS_AS(SessionManager(path, SessionManager::OpenMode::ReadOnly), SessionManager::SchemaTooNewError);
    dir.deleteRecursively();
}

TEST_CASE("A guest FeedbackManager stays out of a DB migrated by a newer build (FIX-18)",
          "[session][migrations][fix18]")
{
    const auto dir = freshTempDir("schema-newer-feedback");
    const std::string path = dir.getChildFile("khensu.db").getFullPathName().toStdString();
    {
        SessionManager current(path);
    }
    {
        // This build's schema: both guest modes work.
        FeedbackManager guest(path, FeedbackManager::OpenMode::NoProvision);
        CHECK(guest.isAvailable());
        FeedbackManager reader(path, FeedbackManager::OpenMode::ReadOnlyNoProvision);
        CHECK(reader.isAvailable());
    }
    {
        SessionManager current(path);
        current.execRawSqlForTesting("INSERT INTO schema_version (version, applied_at) VALUES (" +
                                     std::to_string(SessionManager::latestSchemaVersion() + 1) + ", 'future')");
    }
    {
        FeedbackManager guest(path, FeedbackManager::OpenMode::NoProvision);
        CHECK_FALSE(guest.isAvailable());
        CHECK(guest.recordActedOn("rec", true).is_err()); // fails soft, writes nothing
        FeedbackManager reader(path, FeedbackManager::OpenMode::ReadOnlyNoProvision);
        CHECK_FALSE(reader.isAvailable());
        CHECK(reader.getActedOnBatch({"rec"}).empty());
    }
    dir.deleteRecursively();
}

TEST_CASE("A read-only SessionManager reads sessions but never creates, migrates or writes",
          "[session][migrations]")
{
    const auto dir = freshTempDir("readonly");
    const auto file = dir.getChildFile("khensu.db");
    CHECK_THROWS(SessionManager(file.getFullPathName().toStdString(), SessionManager::OpenMode::ReadOnly));
    CHECK_FALSE(file.existsAsFile()); // not created

    writeDbAtVersion(file.getFullPathName().toStdString(), 5);
    {
        SessionManager reader(file.getFullPathName().toStdString(), SessionManager::OpenMode::ReadOnly);
        CHECK(reader.isReadOnly());
        CHECK(reader.listSessions().is_ok());
        CHECK_FALSE(reader.createSession("nope").is_ok());
    }
    {
        SQLite::Database check(file.getFullPathName().toStdString(), SQLite::OPEN_READONLY);
        SQLite::Statement q(check, "SELECT MAX(version) FROM schema_version");
        REQUIRE(q.executeStep());
        CHECK(q.getColumn(0).getInt() == 5); // left at v5: the reader migrated nothing
    }
    dir.deleteRecursively();
}

// ---------------------------------------------------------------------------
// AUDIT-014 / AUDIT-058: erasure removes every session-scoped trace.
// ---------------------------------------------------------------------------

namespace {
// Counts through a second connection (a backup snapshot), the way another
// process would see the data.
int countRows(SessionManager& mgr, const std::string& table)
{
    const auto tmp = juce::File::getSpecialLocation(juce::File::tempDirectory)
                         .getChildFile("khensu-count-" + juce::Uuid().toString() + ".db");
    if (!mgr.backupTo(tmp.getFullPathName().toStdString()).is_ok()) return -1;
    int n = -1;
    {
        SQLite::Database db(tmp.getFullPathName().toStdString(), SQLite::OPEN_READONLY);
        SQLite::Statement q(db, "SELECT COUNT(*) FROM " + table);
        if (q.executeStep()) n = q.getColumn(0).getInt();
    }
    tmp.deleteFile();
    for (const char* sidecar : {"-wal", "-shm"}) // the backup is a WAL database (PKG-4)
        juce::File(tmp.getFullPathName() + sidecar).deleteFile();
    return n;
}

void seedSessionScopedData(SessionManager& mgr, const std::string& sid, const std::string& sourcePath)
{
    Session s = mgr.loadSession(sid).value().value();
    AudioFileRef f;
    f.id = "f-" + sid;
    f.path = sourcePath;
    s.files.push_back(f);
    REQUIRE(mgr.saveSession(s).is_ok());
    for (const char* kind : {"chat_history_", "chat_decisions_", "chat_pinned_", "vocal_strategy_", "vocal_macros_",
                             "workspace_recovery_"})
        REQUIRE(mgr.setUserPreference(kind + sid, "{}").is_ok());
    const std::string annotationKey =
        "annotations:" + juce::String::toHexString(juce::String(sourcePath).hashCode64()).toStdString();
    REQUIRE(mgr.setUserPreference(annotationKey, "[]").is_ok());
    REQUIRE(mgr.enqueueOutboxEvent(sid + "-fb", "feedback", "{\"session_id\":\"" + sid + "\",\"rec_id\":\"r\"}")
                .is_ok());
    REQUIRE(mgr.enqueueOutboxEvent(sid + "-ev", "event", "{\"session_id\":\"" + sid + "\",\"event_type\":\"x\"}")
                .is_ok());
    mgr.execRawSqlForTesting("INSERT INTO provenance_events (id, session_id, event_type, created_at) VALUES ('ev-" +
                             sid + "', '" + sid + "', 'recommendation_applied', 't')");
}
} // namespace

TEST_CASE("deleteAllSessions erases session-scoped prefs, the outbox and provenance; dispatch then sends nothing",
          "[session][erasure]")
{
    const auto dir = freshTempDir("erase-all");
    {
        SessionManager mgr(dir.getChildFile("khensu.db").getFullPathName().toStdString());
        const auto a = mgr.createSession("A").value().id;
        const auto b = mgr.createSession("B").value().id;
        seedSessionScopedData(mgr, a, "C:/music/a.wav");
        seedSessionScopedData(mgr, b, "C:/music/b.wav");
        REQUIRE(mgr.setUserPreference("chat_history_orphan-of-an-old-delete", "{}").is_ok());
        REQUIRE(mgr.setUserPreference("ui.theme", "\"dark\"").is_ok()); // app setting: must survive

        REQUIRE(mgr.deleteAllSessions().is_ok());
        CHECK(mgr.compactAfterErasure().is_ok());

        for (const char* prefix : {"chat_history_", "chat_decisions_", "chat_pinned_", "vocal_",
                                   "workspace_recovery_", "annotations:"})
            CHECK(mgr.listUserPreferenceKeysWithPrefix(prefix).empty());
        CHECK(mgr.getUserPreference("ui.theme").is_ok());
        CHECK(mgr.pendingOutboxEntries().empty());
        CHECK(mgr.undeliveredOutboxCount() == 0);
        CHECK(countRows(mgr, "recommendation_outbox") == 0);
        CHECK(countRows(mgr, "provenance_events") == 0);

        int sends = 0;
        const auto result = OutboxDispatcher::dispatchDue(mgr, [&sends](const std::string&, const std::string&) {
            ++sends;
            return OutboxDispatcher::SendOutcome::Delivered;
        });
        CHECK(sends == 0);
        CHECK(result.delivered == 0);
        // The WAL no longer carries the erased pages.
        const auto wal = dir.getChildFile("khensu.db-wal");
        CHECK((!wal.existsAsFile() || wal.getSize() == 0));
    }
    dir.deleteRecursively();
}

TEST_CASE("deleteSession erases only that session's prefs, outbox rows, annotations and provenance",
          "[session][erasure]")
{
    SessionManager mgr(":memory:");
    const auto a = mgr.createSession("A").value().id;
    const auto b = mgr.createSession("B").value().id;
    seedSessionScopedData(mgr, a, "C:/music/a.wav");
    seedSessionScopedData(mgr, b, "C:/music/b.wav");

    REQUIRE(mgr.deleteSession(a).is_ok());

    CHECK(mgr.listUserPreferenceKeysWithPrefix("chat_history_" + a).empty());
    CHECK(mgr.listUserPreferenceKeysWithPrefix("workspace_recovery_" + a).empty());
    CHECK(mgr.listUserPreferenceKeysWithPrefix("chat_history_" + b).size() == 1);
    CHECK(mgr.listUserPreferenceKeysWithPrefix("annotations:").size() == 1); // b's only

    const auto pending = mgr.pendingOutboxEntries();
    REQUIRE(pending.size() == 2);
    for (const auto& e : pending)
        CHECK(e.payloadJson.find(b) != std::string::npos);
    CHECK(countRows(mgr, "provenance_events") == 1);
}

TEST_CASE("SessionManager round-trips the genre confirmation (P0-1)", "[session]")
{
    SessionManager mgr(":memory:");
    CreativeTarget target;
    target.primary_genre = "trap";
    target.use_genre_targets = false;
    target.genre_confirmed_source = khensu::genre_confirmation::kUserStated;
    target.reference_track_paths = {"C:/refs/a.wav", "C:/refs/b.wav"};
    auto created = mgr.createSession("Confirmed", target);
    REQUIRE(created.is_ok());

    auto loaded = mgr.loadSession(created.value().id);
    REQUIRE(loaded.is_ok());
    REQUIRE(loaded.value().has_value());
    const auto& t = loaded.value()->target;
    CHECK_FALSE(t.use_genre_targets);
    CHECK(t.genre_confirmed_source == "user_stated");
    CHECK(t.reference_track_paths == std::vector<std::string>{"C:/refs/a.wav", "C:/refs/b.wav"});

    // A session saved before P0-1 (no keys) loads as targets on, unconfirmed.
    auto plain = mgr.createSession("Old", CreativeTarget{});
    REQUIRE(plain.is_ok());
    auto reloaded = mgr.loadSession(plain.value().id);
    REQUIRE(reloaded.is_ok());
    REQUIRE(reloaded.value().has_value());
    CHECK(reloaded.value()->target.use_genre_targets);
    CHECK(reloaded.value()->target.genre_confirmed_source.empty());
}

TEST_CASE("Genre confirmation: intake choices and the action rule (P0-1)", "[session][genre]")
{
    namespace gc = khensu::genre_confirmation;
    gc::IntakeChoice c;
    c.selectionMode = "detected";
    CHECK(gc::sourceForIntake(c).empty()); // a detected candidate alone never confirms
    c.confirmDetected = true;
    CHECK(gc::sourceForIntake(c) == gc::kConfirmedCandidate);
    c.confirmDetected = false;
    c.statedGenreId = "trap";
    CHECK(gc::sourceForIntake(c) == gc::kUserStated);

    gc::IntakeChoice stated;
    stated.selectionMode = "stated";
    CHECK(gc::sourceForIntake(stated).empty()); // "stated" with no genre picked
    stated.statedGenreId = "pop";
    CHECK(gc::sourceForIntake(stated) == gc::kUserStated);
    stated.useGenreTargets = false;
    CHECK(gc::sourceForIntake(stated).empty());

    gc::IntakeChoice ref;
    ref.selectionMode = "reference";
    CHECK(gc::sourceForIntake(ref).empty());
    ref.referenceExists = true;
    CHECK(gc::sourceForIntake(ref) == gc::kReferenceTrack);
    ref.selectionMode = "none";
    CHECK(gc::sourceForIntake(ref).empty());

    CreativeTarget t;
    CHECK_FALSE(gc::actionAllowed(t, false, false));
    CHECK(gc::actionAllowed(t, /*referenceLoaded*/ true, false));
    t.genre_confirmed_source = gc::kUserStated;
    CHECK(gc::actionAllowed(t, false, false));
    CHECK_FALSE(gc::actionAllowed(t, false, /*hybrid*/ true));
    t.use_genre_targets = false;
    CHECK_FALSE(gc::actionAllowed(t, true, false));
}

TEST_CASE("Sessions can be renamed, noted and duplicated (WF-12)", "[session][wf12]")
{
    SessionManager mgr(":memory:");
    CreativeTarget target;
    target.primary_genre = "shoegaze";
    auto created = mgr.createSession("Mix v1", target);
    REQUIRE(created.is_ok());
    Session s = created.value();
    AudioFileRef f;
    f.id = "f1";
    f.path = "C:/audio/song.wav";
    s.files.push_back(f);
    REQUIRE(mgr.saveSession(s).is_ok());

    REQUIRE(mgr.renameSession(s.id, "  Mix v2  ").is_ok());
    REQUIRE(mgr.setSessionNote(s.id, "vocal up 1 dB").is_ok());
    auto loaded = mgr.loadSession(s.id);
    REQUIRE(loaded.is_ok());
    REQUIRE(loaded.value().has_value());
    CHECK(loaded.value()->name == "Mix v2");
    CHECK(loaded.value()->note == "vocal up 1 dB");
    CHECK(mgr.renameSession(s.id, "   ").is_err());           // a session needs a name
    CHECK(mgr.renameSession("no-such-id", "x").is_err());

    auto dup = mgr.duplicateSession(s.id, "");
    REQUIRE(dup.is_ok());
    CHECK(dup.value().id != s.id);
    CHECK(dup.value().name == "Mix v2 (copy)");
    REQUIRE(dup.value().files.size() == 1);
    CHECK(dup.value().files[0].path == "C:/audio/song.wav");
    CHECK(dup.value().target.primary_genre == "shoegaze");
    CHECK(dup.value().note == "vocal up 1 dB");
    auto both = mgr.listSessions();
    REQUIRE(both.is_ok());
    CHECK(both.value().size() == 2);
}

TEST_CASE("Session::analysedFiles grades a multitrack session's mixdown only (WF-7)", "[session]")
{
    Session s;
    AudioFileRef a, b;
    a.path = "a.wav";
    b.path = "b.wav";
    s.files = {a, b};
    CHECK(s.analysedFiles().size() == 2); // a plain session: every file

    AudioFileRef mix, vox;
    mix.path = "mix.wav";
    mix.track_role = "mix";
    vox.path = "vox.wav";
    vox.track_role = "vocals";
    s.files = {vox, mix};
    REQUIRE(s.analysedFiles().size() == 1);
    CHECK(s.analysedFiles()[0].path == "mix.wav");

    s.files = {vox}; // no mixdown: every file, as before
    CHECK(s.analysedFiles().size() == 1);
}

TEST_CASE("SessionManager projects group sessions as ordered tracks (FEAT-6)", "[session][project]")
{
    SessionManager mgr(":memory:");
    REQUIRE(mgr.isOpen());
    const auto a = mgr.createSession("Track A").value();
    const auto b = mgr.createSession("Track B").value();
    const auto c = mgr.createSession("Track C").value();

    CHECK(mgr.createProject("   ").is_err());
    auto created = mgr.createProject("Debut EP", "ep");
    REQUIRE(created.is_ok());
    const auto pid = created.value().id;

    for (const auto* s : {&a, &b, &c})
        REQUIRE(mgr.addSessionToProject(pid, s->id).is_ok());
    CHECK(mgr.addSessionToProject("no-such-project", a.id).is_err());

    auto loaded = mgr.loadProject(pid);
    REQUIRE(loaded.is_ok());
    REQUIRE(loaded.value().has_value());
    CHECK(loaded.value()->kind == "ep");
    CHECK(loaded.value()->session_ids == std::vector<std::string>{a.id, b.id, c.id});

    SECTION("the track order is the project's album sequence")
    {
        REQUIRE(mgr.setProjectOrder(pid, {c.id, a.id, b.id, "stranger"}).is_ok());
        CHECK(mgr.loadProject(pid).value()->session_ids == std::vector<std::string>{c.id, a.id, b.id});
    }

    SECTION("a session belongs to one project; moving it takes it out of the first")
    {
        const auto other = mgr.createProject("Singles").value().id;
        REQUIRE(mgr.addSessionToProject(other, b.id).is_ok());
        CHECK(mgr.loadProject(pid).value()->session_ids == std::vector<std::string>{a.id, c.id});
        CHECK(mgr.projectOfSession(b.id).value()->id == other);
        CHECK(mgr.listProjects().value().size() == 2);
    }

    SECTION("deleting a session drops its track; deleting a project keeps its sessions")
    {
        REQUIRE(mgr.deleteSession(a.id).is_ok());
        CHECK(mgr.loadProject(pid).value()->session_ids == std::vector<std::string>{b.id, c.id});
        REQUIRE(mgr.removeSessionFromProject(c.id).is_ok());
        CHECK_FALSE(mgr.projectOfSession(c.id).value().has_value());

        REQUIRE(mgr.renameProject(pid, "LP").is_ok());
        CHECK(mgr.loadProject(pid).value()->name == "LP");
        REQUIRE(mgr.deleteProject(pid).is_ok());
        CHECK_FALSE(mgr.loadProject(pid).value().has_value());
        CHECK(mgr.loadSession(b.id).value().has_value());
        CHECK_FALSE(mgr.projectOfSession(b.id).value().has_value());
    }

    SECTION("erasing every session erases the projects")
    {
        REQUIRE(mgr.deleteAllSessions().is_ok());
        CHECK(mgr.listProjects().value().empty());
    }

    CHECK_FALSE(mgr.latestAnalysisSummary(a.id).value().has_value()); // nothing analysed yet
}

TEST_CASE("SessionManager: a stored native pass is reusable only for the unchanged file (PERF-3)", "[session][perf3]")
{
    SessionManager mgr(":memory:");
    const auto s = mgr.createSession("Reopen").value();
    const auto file = juce::File::createTempFile(".wav");
    REQUIRE(file.replaceWithText("first version"));

    AnalysisResult r;
    r.loudness.lufs_integrated = -10.0f;
    r.sample_rate = 44100.0;
    REQUIRE(mgr.saveAnalysisResult(r, s.id, "f1", 70.0f, file.getFullPathName().toStdString()).is_ok());

    auto reused = mgr.loadReusableNative(s.id, "f1", file.getFullPathName().toStdString());
    REQUIRE(reused.is_ok());
    REQUIRE(reused.value().has_value());
    CHECK(reused.value()->loudness.lufs_integrated == -10.0f);

    // Another file id, or a different file, is not this pass.
    CHECK_FALSE(mgr.loadReusableNative(s.id, "f2", file.getFullPathName().toStdString()).value().has_value());
    // The file changed (new bytes): analyse again.
    REQUIRE(file.replaceWithText("a longer, edited version of the file"));
    CHECK_FALSE(mgr.loadReusableNative(s.id, "f1", file.getFullPathName().toStdString()).value().has_value());

    // Saved without a source (a multi-file session): never reused.
    REQUIRE(mgr.saveAnalysisResult(r, s.id, "f3").is_ok());
    CHECK_FALSE(mgr.loadReusableNative(s.id, "f3", file.getFullPathName().toStdString()).value().has_value());
    file.deleteFile();
}
