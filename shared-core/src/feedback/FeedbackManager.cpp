// THREAD_CONTEXT(WorkerThread|UIThread) -- all methods perform SQLite I/O.
#include <khensu/feedback/FeedbackManager.h>
#include <khensu/session/SessionManager.h>

#include <juce_core/juce_core.h>
#include <SQLiteCpp/SQLiteCpp.h>
#include <mutex>
#include <stdexcept>

namespace khensu {

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------

struct FeedbackManager::Impl {
    std::unique_ptr<SQLite::Database> dbPtr;
    std::mutex mutex;
    std::string path;
    OpenMode mode;
    std::string openError;

    Impl(const std::string& dbPath, OpenMode openMode) : path(dbPath), mode(openMode)
    {
        if (mode == OpenMode::Provision) {
            dbPtr = std::make_unique<SQLite::Database>(path, SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
            provision(*dbPtr);
            return;
        }
        // CORE-16: a guest process (the VST3 plugin) never creates the file,
        // runs DDL, switches the journal mode or imports legacy stores - the
        // desktop app's migration chain owns khensu.db. Opening a missing or
        // locked database is not an error: every call then fails soft.
        try {
            const int flags = mode == OpenMode::ReadOnlyNoProvision ? SQLite::OPEN_READONLY : SQLite::OPEN_READWRITE;
            dbPtr = std::make_unique<SQLite::Database>(path, flags);
            dbPtr->exec("PRAGMA busy_timeout=5000"); // per-connection, writes nothing
            // FIX-18: a guest cannot migrate, so it stays out of a database a
            // newer build has migrated - its tables may have moved.
            SessionManager::throwIfSchemaTooNew(*dbPtr);
        } catch (const std::exception& e) {
            dbPtr.reset();
            openError = e.what();
            juce::Logger::writeToLog("FeedbackManager: shared database unavailable (" + juce::String(openError) +
                                     ") - feedback calls will fail soft");
        }
    }

    /// The open connection, or throws (caught by every public method, which
    /// then returns an error) when a no-provision open failed.
    SQLite::Database& database()
    {
        if (dbPtr == nullptr)
            throw std::runtime_error("feedback store unavailable" + (openError.empty() ? std::string() : ": " + openError));
        return *dbPtr;
    }

    void provision(SQLite::Database& db)
    {
        db.exec("PRAGMA journal_mode=WAL");
        db.exec("PRAGMA synchronous=NORMAL");     // WAL mode: NORMAL is safe and fast
        db.exec("PRAGMA wal_autocheckpoint=200"); // checkpoint every 200 pages (~1.5 MB)
        db.exec("PRAGMA busy_timeout=5000");
        // ENG-8: the store's tables come from the migration chain itself -
        // 008 recommendation_feedback, 009 parameter_overrides, 010
        // recommendation_prefs (NOT `user_preferences`: on khensu.db that is
        // migration 004's JSON key/value store) and 012
        // preference_learning_events - all written `IF NOT EXISTS`, so running
        // them here when this manager opens khensu.db before SessionManager
        // has is safe. No second copy of their DDL to drift.
        for (const auto& m : SessionManager::migrations())
            if (m.version == 8 || m.version == 9 || m.version == 10 || m.version == 12) db.exec(m.sql);
        importLegacyFeedbackDb();
    }

    // One-time, backup-first, marker-guarded import of the legacy feedback.db
    // into the unified khensu.db (ARCH-03 Phase 3; mirrors the Python
    // import_once helper). Best-effort: any failure is swallowed so it can never
    // block app startup, and the unset marker simply retries next launch. The
    // old file is backed up (*.bak) and renamed (*.migrated), never deleted.
    void importLegacyFeedbackDb()
    {
        SQLite::Database& db = *dbPtr;
        try {
            if (path == ":memory:") return;
            const juce::File dbFile{juce::String(path)};
            const juce::File legacy = dbFile.getParentDirectory().getChildFile("feedback.db");
            if (dbFile.getFullPathName() == legacy.getFullPathName())
                return;   // still pointed at the old file (shouldn't happen post-Phase-3)

            db.exec("CREATE TABLE IF NOT EXISTS data_migrations ("
                    "name TEXT PRIMARY KEY, applied_at TEXT NOT NULL)");
            {
                SQLite::Statement done(db, "SELECT 1 FROM data_migrations WHERE name='feedback_from_feedback_db'");
                if (done.executeStep()) return;
            }
            const std::string now = juce::Time::getCurrentTime().toISO8601(true).toStdString();
            auto markDone = [&] {
                SQLite::Statement m(db, "INSERT OR IGNORE INTO data_migrations (name, applied_at) VALUES ('feedback_from_feedback_db', ?)");
                m.bind(1, now);
                m.exec();
            };

            if (!legacy.existsAsFile()) { markDone(); return; }

            // AUDIT-059: fold any uncheckpointed WAL into the main file first -
            // copying/attaching just feedback.db would miss committed rows that
            // still live only in feedback.db-wal. Closing the last connection
            // of a WAL database also removes the -wal/-shm sidecars.
            {
                SQLite::Database legacyDb(legacy.getFullPathName().toStdString(), SQLite::OPEN_READWRITE);
                legacyDb.exec("PRAGMA busy_timeout=5000");
                legacyDb.exec("PRAGMA wal_checkpoint(TRUNCATE)");
            }

            // Backup-first means the import does not run unless the backup
            // (and any sidecar the checkpoint couldn't remove) really exists.
            const juce::File bak(legacy.getFullPathName() + ".bak");
            if (!bak.existsAsFile() && !legacy.copyFileTo(bak)) {
                juce::Logger::writeToLog("FeedbackManager: legacy feedback.db backup failed - import deferred");
                return; // marker unset: retried next launch
            }
            for (const char* sidecar : {"-wal", "-shm"}) {
                const juce::File side(legacy.getFullPathName() + sidecar);
                const juce::File sideBak(bak.getFullPathName() + sidecar);
                if (side.existsAsFile() && !sideBak.existsAsFile() && !side.copyFileTo(sideBak)) {
                    juce::Logger::writeToLog("FeedbackManager: legacy feedback.db sidecar backup failed - import deferred");
                    return;
                }
            }

            SQLite::Statement att(db, "ATTACH DATABASE ? AS src");
            att.bind(1, legacy.getFullPathName().toStdString());
            att.exec();
            try {
                SQLite::Transaction tx(db);
                // recommendation_feedback: last-write-wins on updated_at (may
                // already hold service-side rows imported in Phase 2).
                db.exec("INSERT INTO recommendation_feedback "
                        "(id,recommendation_id,acted_on,feedback_note,created_at,updated_at) "
                        "SELECT id,recommendation_id,acted_on,feedback_note,created_at,updated_at "
                        "FROM src.recommendation_feedback WHERE true "
                        "ON CONFLICT(recommendation_id) DO UPDATE SET "
                        "acted_on=excluded.acted_on, feedback_note=excluded.feedback_note, "
                        "updated_at=excluded.updated_at "
                        "WHERE excluded.updated_at > recommendation_feedback.updated_at");
                db.exec("INSERT OR IGNORE INTO parameter_overrides "
                        "(id,recommendation_id,parameter_name,recommended_value,actual_value,created_at) "
                        "SELECT id,recommendation_id,parameter_name,recommended_value,actual_value,created_at "
                        "FROM src.parameter_overrides");
                // feedback.db's `user_preferences` (numeric) -> recommendation_prefs.
                db.exec("INSERT OR IGNORE INTO recommendation_prefs (pref_key,pref_value) "
                        "SELECT pref_key,pref_value FROM src.user_preferences");
                markDone();
                tx.commit();
            } catch (...) {
                try { db.exec("DETACH DATABASE src"); } catch (...) {}
                throw;
            }
            db.exec("DETACH DATABASE src");

            // Retire the file together with its sidecars (named so SQLite
            // would still pair them if the .migrated file were reopened).
            const juce::File retired(legacy.getFullPathName() + ".migrated");
            if (!legacy.moveFileTo(retired))
                juce::Logger::writeToLog("FeedbackManager: could not retire legacy feedback.db (imported; marker set)");
            for (const char* sidecar : {"-wal", "-shm"}) {
                const juce::File side(legacy.getFullPathName() + sidecar);
                if (side.existsAsFile()) side.moveFileTo(juce::File(retired.getFullPathName() + sidecar));
            }
        } catch (const std::exception& e) {
            juce::Logger::writeToLog(juce::String("FeedbackManager: legacy feedback.db import deferred - ") + e.what());
        }
    }
};

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

FeedbackManager::FeedbackManager(const std::string& db_path, OpenMode mode)
    : impl_(std::make_unique<Impl>(db_path, mode))
{}

bool FeedbackManager::isAvailable() const noexcept { return impl_->dbPtr != nullptr; }

FeedbackManager::~FeedbackManager() = default;

// ---------------------------------------------------------------------------
// recordActedOn
// ---------------------------------------------------------------------------

Result<void> FeedbackManager::recordActedOn(const std::string& recommendation_id, bool acted_on,
                                            const std::string& note)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    try {
        const std::string now = juce::Time::getCurrentTime().toISO8601(true).toStdString();

        // Check whether a row already exists for this recommendation_id.
        SQLite::Statement check(impl_->database(), "SELECT id FROM recommendation_feedback WHERE recommendation_id = ?");
        check.bind(1, recommendation_id);

        if (check.executeStep()) {
            SQLite::Statement upd(impl_->database(), "UPDATE recommendation_feedback "
                                             "SET acted_on=?, feedback_note=?, updated_at=? "
                                             "WHERE recommendation_id=?");
            upd.bind(1, acted_on ? 1 : 0);
            upd.bind(2, note);
            upd.bind(3, now);
            upd.bind(4, recommendation_id);
            upd.exec();
        } else {
            const std::string id = juce::Uuid().toString().toStdString();
            SQLite::Statement ins(impl_->database(), "INSERT INTO recommendation_feedback "
                                             "(id, recommendation_id, acted_on, feedback_note, created_at, updated_at) "
                                             "VALUES (?,?,?,?,?,?)");
            ins.bind(1, id);
            ins.bind(2, recommendation_id);
            ins.bind(3, acted_on ? 1 : 0);
            ins.bind(4, note);
            ins.bind(5, now);
            ins.bind(6, now);
            ins.exec();
        }

        return Result<void>::ok();
    } catch (const std::exception& e) {
        return Result<void>::err(std::string("FeedbackManager: ") + e.what());
    }
}

// ---------------------------------------------------------------------------
// getActedOn
// ---------------------------------------------------------------------------

Result<bool> FeedbackManager::getActedOn(const std::string& recommendation_id)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    try {
        SQLite::Statement sel(impl_->database(), "SELECT acted_on FROM recommendation_feedback WHERE recommendation_id = ?");
        sel.bind(1, recommendation_id);
        if (sel.executeStep()) {
            return Result<bool>::ok(sel.getColumn(0).getInt() != 0);
        }
        return Result<bool>::err("No feedback record found for recommendation: " + recommendation_id);
    } catch (const std::exception& e) {
        return Result<bool>::err(std::string("FeedbackManager: ") + e.what());
    }
}

std::map<std::string, bool> FeedbackManager::getActedOnBatch(const std::vector<std::string>& recommendation_ids)
{
    std::map<std::string, bool> result;
    if (recommendation_ids.empty()) return result;

    std::lock_guard<std::mutex> lock(impl_->mutex);
    try {
        std::string placeholders(recommendation_ids.size() * 2 - 1, '?');
        for (size_t i = 1; i < recommendation_ids.size(); ++i)
            placeholders[i * 2 - 1] = ',';

        SQLite::Statement sel(impl_->database(), "SELECT recommendation_id, acted_on FROM recommendation_feedback "
                                         "WHERE recommendation_id IN (" +
                                             placeholders + ")");
        for (size_t i = 0; i < recommendation_ids.size(); ++i)
            sel.bind(static_cast<int>(i) + 1, recommendation_ids[i]);

        while (sel.executeStep())
            result[sel.getColumn(0).getString()] = sel.getColumn(1).getInt() != 0;
    } catch (const std::exception&) {
        // Best-effort - callers already treat a missing id as not-acted-on.
    }
    return result;
}

std::map<std::string, FeedbackRow>
FeedbackManager::getFeedbackBatch(const std::vector<std::string>& recommendation_ids)
{
    std::map<std::string, FeedbackRow> result;
    if (recommendation_ids.empty()) return result;

    std::lock_guard<std::mutex> lock(impl_->mutex);
    try {
        std::string placeholders(recommendation_ids.size() * 2 - 1, '?');
        for (size_t i = 1; i < recommendation_ids.size(); ++i)
            placeholders[i * 2 - 1] = ',';

        SQLite::Statement sel(impl_->database(),
                              "SELECT recommendation_id, acted_on, COALESCE(feedback_note, '') "
                              "FROM recommendation_feedback WHERE recommendation_id IN (" +
                                  placeholders + ")");
        for (size_t i = 0; i < recommendation_ids.size(); ++i)
            sel.bind(static_cast<int>(i) + 1, recommendation_ids[i]);

        while (sel.executeStep()) {
            FeedbackRow row;
            row.acted_on = sel.getColumn(1).getInt() != 0;
            row.note = sel.getColumn(2).getString();
            result[sel.getColumn(0).getString()] = std::move(row);
        }
    } catch (const std::exception&) {
        // Best-effort, like getActedOnBatch: a missing id reads as untouched.
    }
    return result;
}

Result<void> FeedbackManager::setPreference(const std::string& key, float val)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    try {
        SQLite::Statement upd(impl_->database(), "INSERT INTO recommendation_prefs (pref_key, pref_value) "
                                         "VALUES (?, ?) "
                                         "ON CONFLICT(pref_key) DO UPDATE SET pref_value=excluded.pref_value");
        upd.bind(1, key);
        upd.bind(2, static_cast<double>(val));
        upd.exec();
        return Result<void>::ok();
    } catch (const std::exception& e) {
        return Result<void>::err(std::string("FeedbackManager::setPreference: ") + e.what());
    }
}

Result<float> FeedbackManager::getPreference(const std::string& key, float defaultVal)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    try {
        SQLite::Statement select(impl_->database(), "SELECT pref_value FROM recommendation_prefs WHERE pref_key = ?");
        select.bind(1, key);
        if (select.executeStep()) {
            return Result<float>::ok(static_cast<float>(select.getColumn(0).getDouble()));
        }
        return Result<float>::ok(defaultVal);
    } catch (const std::exception& e) {
        return Result<float>::err(std::string("FeedbackManager::getPreference: ") + e.what());
    }
}

// ---------------------------------------------------------------------------
// recordOverride  (#25 audit - parameter override telemetry)
// ---------------------------------------------------------------------------

Result<void> FeedbackManager::recordOverride(const std::string& recommendation_id, const std::string& parameter_name,
                                             float recommended_value, float actual_value)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    try {
        const std::string id = juce::Uuid().toString().toStdString();
        const std::string now = juce::Time::getCurrentTime().toISO8601(true).toStdString();
        SQLite::Statement ins(impl_->database(),
                              "INSERT INTO parameter_overrides "
                              "(id, recommendation_id, parameter_name, recommended_value, actual_value, created_at) "
                              "VALUES (?,?,?,?,?,?)");
        ins.bind(1, id);
        ins.bind(2, recommendation_id);
        ins.bind(3, parameter_name);
        ins.bind(4, static_cast<double>(recommended_value));
        ins.bind(5, static_cast<double>(actual_value));
        ins.bind(6, now);
        ins.exec();
        return Result<void>::ok();
    } catch (const std::exception& e) {
        return Result<void>::err(std::string("FeedbackManager::recordOverride: ") + e.what());
    }
}

// ---------------------------------------------------------------------------
// listOverrides
// ---------------------------------------------------------------------------

Result<std::vector<ParameterOverride>> FeedbackManager::listOverrides(const std::string& recommendation_id)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    try {
        SQLite::Statement sel(impl_->database(),
                              "SELECT recommendation_id, parameter_name, recommended_value, actual_value, created_at "
                              "FROM parameter_overrides WHERE recommendation_id = ? ORDER BY created_at DESC");
        sel.bind(1, recommendation_id);

        std::vector<ParameterOverride> out;
        while (sel.executeStep()) {
            ParameterOverride ov;
            ov.recommendation_id = sel.getColumn(0).getString();
            ov.parameter_name = sel.getColumn(1).getString();
            ov.recommended_value = static_cast<float>(sel.getColumn(2).getDouble());
            ov.actual_value = static_cast<float>(sel.getColumn(3).getDouble());
            ov.created_at = sel.getColumn(4).getString();
            out.push_back(std::move(ov));
        }
        return Result<std::vector<ParameterOverride>>::ok(std::move(out));
    } catch (const std::exception& e) {
        return Result<std::vector<ParameterOverride>>::err(std::string("FeedbackManager::listOverrides: ") + e.what());
    }
}

// ---------------------------------------------------------------------------
// Learned taste evidence
// ---------------------------------------------------------------------------

Result<void> FeedbackManager::recordLearningEvent(const PreferenceLearningEvent& event)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    try {
        const std::string now = event.created_at.empty()
                                    ? juce::Time::getCurrentTime().toISO8601(true).toStdString()
                                    : event.created_at;
        SQLite::Statement ins(impl_->database(),
                              "INSERT INTO preference_learning_events "
                              "(pref_key, source, recommendation_id, accepted, value_before, value_after, created_at) "
                              "VALUES (?,?,?,?,?,?,?)");
        ins.bind(1, event.pref_key);
        ins.bind(2, event.source);
        ins.bind(3, event.recommendation_id);
        ins.bind(4, event.accepted ? 1 : 0);
        ins.bind(5, static_cast<double>(event.value_before));
        ins.bind(6, static_cast<double>(event.value_after));
        ins.bind(7, now);
        ins.exec();
        return Result<void>::ok();
    } catch (const std::exception& e) {
        return Result<void>::err(std::string("FeedbackManager::recordLearningEvent: ") + e.what());
    }
}

Result<std::vector<PreferenceLearningEvent>> FeedbackManager::listLearningEvents()
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    try {
        SQLite::Statement sel(impl_->database(),
                              "SELECT pref_key, source, recommendation_id, accepted, value_before, value_after, "
                              "created_at FROM preference_learning_events ORDER BY id ASC");
        std::vector<PreferenceLearningEvent> out;
        while (sel.executeStep()) {
            PreferenceLearningEvent ev;
            ev.pref_key = sel.getColumn(0).getString();
            ev.source = sel.getColumn(1).getString();
            ev.recommendation_id = sel.getColumn(2).getString();
            ev.accepted = sel.getColumn(3).getInt() != 0;
            ev.value_before = static_cast<float>(sel.getColumn(4).getDouble());
            ev.value_after = static_cast<float>(sel.getColumn(5).getDouble());
            ev.created_at = sel.getColumn(6).getString();
            out.push_back(std::move(ev));
        }
        return Result<std::vector<PreferenceLearningEvent>>::ok(std::move(out));
    } catch (const std::exception& e) {
        return Result<std::vector<PreferenceLearningEvent>>::err(
            std::string("FeedbackManager::listLearningEvents: ") + e.what());
    }
}

Result<void> FeedbackManager::resetLearnedPreference(const std::string& pref_key)
{
    return resetLearnedPreferences({pref_key});
}

Result<void> FeedbackManager::resetLearnedPreferences(const std::vector<std::string>& pref_keys)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    try {
        SQLite::Transaction tx(impl_->database());
        for (const auto& key : pref_keys) {
            SQLite::Statement delPref(impl_->database(), "DELETE FROM recommendation_prefs WHERE pref_key = ?");
            delPref.bind(1, key);
            delPref.exec();
            SQLite::Statement delLog(impl_->database(), "DELETE FROM preference_learning_events WHERE pref_key = ?");
            delLog.bind(1, key);
            delLog.exec();
        }
        tx.commit();
        return Result<void>::ok();
    } catch (const std::exception& e) {
        return Result<void>::err(std::string("FeedbackManager::resetLearnedPreferences: ") + e.what());
    }
}

Result<void> FeedbackManager::deleteAll()
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    try {
        // recommendation_prefs, not user_preferences: on the unified khensu.db
        // the latter is an unrelated JSON key/value store (theme / brain state)
        // that "delete all feedback" must NOT wipe.
        SQLite::Transaction tx(impl_->database());
        impl_->database().exec("DELETE FROM recommendation_feedback");
        impl_->database().exec("DELETE FROM recommendation_prefs");
        impl_->database().exec("DELETE FROM parameter_overrides");
        impl_->database().exec("DELETE FROM preference_learning_events");
        tx.commit();
        return Result<void>::ok();
    } catch (const std::exception& e) {
        return Result<void>::err(std::string("FeedbackManager::deleteAll: ") + e.what());
    }
}

Result<void> FeedbackManager::checkpoint(bool force)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    try {
        if (!force && impl_->path != ":memory:") {
            juce::File walFile(impl_->path + "-wal");
            if (walFile.existsAsFile()) {
                // Only checkpoint if WAL exceeds 10 MB
                if (walFile.getSize() < 10LL * 1024LL * 1024LL) {
                    return Result<void>::ok();
                }
            }
        }
        // TRUNCATE, not PASSIVE (AUDIT-087): a passive checkpoint copies pages
        // back but never shrinks the -wal file, so once it passed 10 MB it
        // stayed there and this ran on every tick. TRUNCATE waits (busy
        // timeout) for readers and resets the WAL to zero bytes.
        impl_->database().exec("PRAGMA wal_checkpoint(TRUNCATE)");
        return Result<void>::ok();
    } catch (const std::exception& e) {
        return Result<void>::err(std::string("FeedbackManager::checkpoint: ") + e.what());
    }
}

} // namespace khensu
