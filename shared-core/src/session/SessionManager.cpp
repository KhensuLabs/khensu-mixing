// THREAD_CONTEXT(WorkerThread) -- all methods perform SQLite I/O.
#include <khensu/session/SessionManager.h>
#include <khensu/mixerchain/PluginScanner.h>
#include <khensu/analysis/AnalysisResultJson.h>

#include <juce_core/juce_core.h>
#include <SQLiteCpp/SQLiteCpp.h>
#include <SQLiteCpp/Backup.h>

#include <algorithm>
#include <stdexcept>

namespace khensu {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static std::string varToJsonString(const juce::var& v) { return juce::JSON::toString(v, false).toStdString(); }

static juce::var parseJsonString(const std::string& s) { return juce::JSON::parse(juce::String(s)); }

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

SessionManager::SessionManager(const std::string& dbPath, OpenMode mode) : m_readOnly(mode == OpenMode::ReadOnly)
{
    if (m_readOnly) {
        // Readers (the VST3 plugin) never migrate or create the file: every
        // plugin instance used to construct a read/write manager and race the
        // desktop app's migration runner (AUDIT-013).
        m_db = std::make_unique<SQLite::Database>(dbPath, SQLite::OPEN_READONLY);
        m_db->exec("PRAGMA busy_timeout=5000");
        // FIX-18: a reader that cannot migrate must not read a newer layout
        // as if it were its own.
        throwIfSchemaTooNew(*m_db);
        return;
    }
    const int flags = SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE;
    m_db = std::make_unique<SQLite::Database>(dbPath, flags);
    m_db->exec("PRAGMA busy_timeout=5000");
    m_db->exec("PRAGMA journal_mode=WAL");
    m_db->exec("PRAGMA foreign_keys=ON");
    // Erased rows are overwritten on disk rather than left in free pages
    // (AUDIT-014); compactAfterErasure() handles the WAL and the file size.
    m_db->exec("PRAGMA secure_delete=ON");
    runMigrations();
}

SessionManager::~SessionManager() = default;
SessionManager::SessionManager(SessionManager&&) noexcept = default;
SessionManager& SessionManager::operator=(SessionManager&&) noexcept = default;

bool SessionManager::isOpen() const noexcept { return m_db != nullptr; }

bool SessionManager::isReadOnly() const noexcept { return m_readOnly; }

void SessionManager::execRawSqlForTesting(const std::string& sql)
{
    const std::lock_guard<std::recursive_mutex> dbLock(*m_dbMutex);
    m_db->exec(sql);
}

// ---------------------------------------------------------------------------
// Migrations
// ---------------------------------------------------------------------------

const std::vector<SessionManager::Migration>& SessionManager::migrations()
{
    // Single source of truth: the entries below are GENERATED at CMake
    // configure time from storage/schema/NNN_*.sql (audit ARCH-07 - see
    // cmake/GenerateMigrations.cmake). Add or change a migration by editing
    // those .sql files, never this list.
    static const std::vector<Migration> kMigrations = {
#include "khensu_migrations.inc"
    };
    return kMigrations;
}

int SessionManager::latestSchemaVersion()
{
    const auto& all = migrations();
    return all.empty() ? 0 : all.back().version;
}

SessionManager::SchemaTooNewError::SchemaTooNewError(int dbVer, int supportedVer)
    : std::runtime_error("khensu.db schema version " + std::to_string(dbVer) +
                         " is newer than this build supports (" + std::to_string(supportedVer) + ")"),
      dbVersion(dbVer), supportedVersion(supportedVer)
{}

void SessionManager::throwIfSchemaTooNew(SQLite::Database& db)
{
    if (!db.tableExists("schema_version")) return;
    int current = 0;
    SQLite::Statement q(db, "SELECT COALESCE(MAX(version), 0) FROM schema_version");
    if (q.executeStep()) current = q.getColumn(0).getInt();
    if (current > latestSchemaVersion()) throw SchemaTooNewError(current, latestSchemaVersion());
}

void SessionManager::runMigrations()
{
    // AUDIT-013: the desktop app, every DAW plugin instance and the Python
    // service can open khensu.db at the same moment after an upgrade. The
    // version used to be read OUTSIDE any transaction and each migration then
    // ran in a DEFERRED one, so all of them saw "11", one committed 012 and the
    // rest died on the PK / duplicate-column error. BEGIN IMMEDIATE takes the
    // write lock up front (waiting via busy_timeout), the version is re-read
    // INSIDE it, and the whole chain commits atomically - concurrent migrators
    // serialise and the losers find nothing left to apply.
    SQLite::Transaction tx(*m_db, SQLite::TransactionBehavior::IMMEDIATE);
    m_db->exec(R"(
        CREATE TABLE IF NOT EXISTS schema_version (
            version    INTEGER PRIMARY KEY,
            applied_at TEXT    NOT NULL
        )
    )");

    int current = 0;
    {
        SQLite::Statement q(*m_db, "SELECT COALESCE(MAX(version), 0) FROM schema_version");
        if (q.executeStep()) current = q.getColumn(0).getInt();
    }
    // AUDIT-087: a DB migrated by a newer build must not be written by this
    // one (its tables may have moved on); the caller runs degraded instead.
    if (current > latestSchemaVersion()) throw SchemaTooNewError(current, latestSchemaVersion());

    const std::string now = utcNow();
    for (const auto& m : migrations()) {
        if (m.version <= current) continue;
        m_db->exec(m.sql);
        SQLite::Statement ins(*m_db, "INSERT INTO schema_version (version, applied_at) VALUES (?, ?)");
        ins.bind(1, m.version);
        ins.bind(2, now);
        ins.exec();
    }
    tx.commit();
}

// ---------------------------------------------------------------------------
// Serialisation helpers
// ---------------------------------------------------------------------------

std::string SessionManager::generateId() const { return juce::Uuid().toString().toStdString(); }

std::string SessionManager::utcNow() const { return juce::Time::getCurrentTime().toISO8601(true).toStdString(); }

std::string SessionManager::sessionToJson(const Session& s) const
{
    auto root = std::make_unique<juce::DynamicObject>();
    root->setProperty("id", juce::String(s.id));
    root->setProperty("name", juce::String(s.name));
    root->setProperty("created_at", juce::String(s.created_at));
    root->setProperty("updated_at", juce::String(s.updated_at));
    if (!s.note.empty()) root->setProperty("note", juce::String::fromUTF8(s.note.c_str())); // WF-12

    auto tgt = std::make_unique<juce::DynamicObject>();
    tgt->setProperty("primary_genre", juce::String(s.target.primary_genre));
    tgt->setProperty("sub_genre", juce::String(s.target.sub_genre));
    tgt->setProperty("genre_blend", juce::String(s.target.genre_blend));
    tgt->setProperty("mood", juce::String(s.target.mood));
    tgt->setProperty("energy", s.target.energy);
    tgt->setProperty("bpm", s.target.bpm);
    tgt->setProperty("playback_intent", juce::String(s.target.playback_intent));
    tgt->setProperty("hook_emphasis", s.target.hook_emphasis);
    tgt->setProperty("chorus_payoff", s.target.chorus_payoff);
    tgt->setProperty("heaviness_target", s.target.heaviness_target);
    tgt->setProperty("memorability_target", s.target.memorability_target);
    tgt->setProperty("vision_statement", juce::String(s.target.vision_statement));
    tgt->setProperty("run_deep_analysis", s.target.run_deep_analysis);
    tgt->setProperty("use_genre_targets", s.target.use_genre_targets);
    tgt->setProperty("genre_confirmed_source", juce::String(s.target.genre_confirmed_source));
    if (!s.target.reference_track_paths.empty()) {
        juce::Array<juce::var> refs;
        for (const auto& path : s.target.reference_track_paths)
            refs.add(juce::String(path));
        tgt->setProperty("reference_track_paths", juce::var(refs));
    }
    root->setProperty("target", juce::var(tgt.release()));

    juce::Array<juce::var> filesArr;
    for (const auto& f : s.files) {
        auto fo = std::make_unique<juce::DynamicObject>();
        fo->setProperty("id", juce::String(f.id));
        fo->setProperty("path", juce::String(f.path));
        fo->setProperty("format", juce::String(f.format));
        fo->setProperty("duration_seconds", f.duration_seconds);
        fo->setProperty("sample_rate", f.sample_rate);
        fo->setProperty("channels", f.channels);
        fo->setProperty("bit_depth", f.bit_depth);
        fo->setProperty("title", juce::String(f.title));
        fo->setProperty("artist", juce::String(f.artist));
        fo->setProperty("album", juce::String(f.album));
        // Multitrack sessions only - ordinary files keep the old JSON shape.
        if (!f.track_name.empty()) fo->setProperty("track_name", juce::String(f.track_name));
        if (!f.track_role.empty()) fo->setProperty("track_role", juce::String(f.track_role));
        filesArr.add(juce::var(fo.release()));
    }
    root->setProperty("files", juce::var(filesArr));

    return varToJsonString(juce::var(root.release()));
}

Session SessionManager::sessionFromJson(const std::string& jsonStr) const
{
    juce::var v = parseJsonString(jsonStr);
    Session s;
    s.id = v["id"].toString().toStdString();
    s.name = v["name"].toString().toStdString();
    s.created_at = v["created_at"].toString().toStdString();
    s.updated_at = v["updated_at"].toString().toStdString();
    s.note = v["note"].toString().toStdString(); // WF-12; absent in older sessions

    juce::var t = v["target"];
    s.target.primary_genre = t["primary_genre"].toString().toStdString();
    s.target.sub_genre = t["sub_genre"].toString().toStdString();
    s.target.genre_blend = t["genre_blend"].toString().toStdString();
    s.target.mood = t["mood"].toString().toStdString();
    s.target.energy = static_cast<float>(static_cast<double>(t["energy"]));
    s.target.bpm = static_cast<float>(static_cast<double>(t["bpm"]));
    s.target.playback_intent = t["playback_intent"].toString().toStdString();
    s.target.hook_emphasis = static_cast<float>(static_cast<double>(t["hook_emphasis"]));
    s.target.chorus_payoff = static_cast<float>(static_cast<double>(t["chorus_payoff"]));
    s.target.heaviness_target = static_cast<float>(static_cast<double>(t["heaviness_target"]));
    s.target.memorability_target = static_cast<float>(static_cast<double>(t["memorability_target"]));
    s.target.vision_statement = t["vision_statement"].toString().toStdString();
    s.target.run_deep_analysis = t.hasProperty("run_deep_analysis") ? static_cast<bool>(t["run_deep_analysis"]) : true;
    // Sessions saved before P0-1 have neither key: targets on, unconfirmed.
    s.target.use_genre_targets = t.hasProperty("use_genre_targets") ? static_cast<bool>(t["use_genre_targets"]) : true;
    s.target.genre_confirmed_source = t["genre_confirmed_source"].toString().toStdString();
    if (const auto* refs = t["reference_track_paths"].getArray())
        for (const auto& path : *refs)
            s.target.reference_track_paths.push_back(path.toString().toStdString());

    if (const auto* arr = v["files"].getArray()) {
        for (const auto& fv : *arr) {
            AudioFileRef f;
            f.id = fv["id"].toString().toStdString();
            f.path = fv["path"].toString().toStdString();
            f.format = fv["format"].toString().toStdString();
            f.duration_seconds = static_cast<double>(fv["duration_seconds"]);
            f.sample_rate = static_cast<int>(fv["sample_rate"]);
            f.channels = static_cast<int>(fv["channels"]);
            f.bit_depth = static_cast<int>(fv["bit_depth"]);
            f.title = fv["title"].toString().toStdString();
            f.artist = fv["artist"].toString().toStdString();
            f.album = fv["album"].toString().toStdString();
            f.track_name = fv["track_name"].toString().toStdString();
            f.track_role = fv["track_role"].toString().toStdString();
            s.files.push_back(std::move(f));
        }
    }

    return s;
}

// ---------------------------------------------------------------------------
// CRUD
// ---------------------------------------------------------------------------

Result<Session> SessionManager::createSession(const std::string& name, const CreativeTarget& target)
{
    const std::lock_guard<std::recursive_mutex> dbLock(*m_dbMutex);
    if (!m_db) return Result<Session>::err("Database not open");
    Session s;
    s.id = generateId();
    s.name = name;
    s.created_at = utcNow();
    s.updated_at = s.created_at;
    s.target = target;

    try {
        SQLite::Statement stmt(*m_db, "INSERT INTO sessions (id, name, created_at, updated_at, data_json) "
                                      "VALUES (?, ?, ?, ?, ?)");
        stmt.bind(1, s.id);
        stmt.bind(2, s.name);
        stmt.bind(3, s.created_at);
        stmt.bind(4, s.updated_at);
        stmt.bind(5, sessionToJson(s));
        stmt.exec();
        return Result<Session>::ok(std::move(s));
    } catch (const SQLite::Exception& e) {
        return Result<Session>::err(std::string("SQLite error: ") + e.what());
    }
}

Result<std::optional<Session>> SessionManager::loadSession(const std::string& id)
{
    const std::lock_guard<std::recursive_mutex> dbLock(*m_dbMutex);
    if (!m_db) return Result<std::optional<Session>>::err("Database not open");
    try {
        SQLite::Statement stmt(*m_db, "SELECT data_json FROM sessions WHERE id = ?");
        stmt.bind(1, id);
        if (stmt.executeStep())
            return Result<std::optional<Session>>::ok(sessionFromJson(stmt.getColumn(0).getString()));
        return Result<std::optional<Session>>::ok(std::nullopt); // genuinely not found
    } catch (const SQLite::Exception& e) {
        return Result<std::optional<Session>>::err(std::string("SQLite error: ") + e.what());
    }
}

Result<void> SessionManager::saveSession(Session& session)
{
    const std::lock_guard<std::recursive_mutex> dbLock(*m_dbMutex);
    if (!m_db) return Result<void>::err("Database not open");
    session.updated_at = utcNow();
    try {
        SQLite::Statement stmt(*m_db, "UPDATE sessions SET name=?, updated_at=?, data_json=? WHERE id=?");
        stmt.bind(1, session.name);
        stmt.bind(2, session.updated_at);
        stmt.bind(3, sessionToJson(session));
        stmt.bind(4, session.id);
        stmt.exec();
        return Result<void>::ok();
    } catch (const SQLite::Exception& e) {
        return Result<void>::err(std::string("SQLite error: ") + e.what());
    }
}

Result<void> SessionManager::renameSession(const std::string& id, const std::string& name)
{
    const std::lock_guard<std::recursive_mutex> dbLock(*m_dbMutex);
    if (juce::String(name).trim().isEmpty()) return Result<void>::err("A session needs a name");
    auto loaded = loadSession(id);
    if (loaded.is_err()) return Result<void>::err(loaded.error());
    if (!loaded.value()) return Result<void>::err("No such session");
    Session s = *loaded.value();
    s.name = juce::String(name).trim().toStdString();
    return saveSession(s);
}

Result<void> SessionManager::setSessionNote(const std::string& id, const std::string& note)
{
    const std::lock_guard<std::recursive_mutex> dbLock(*m_dbMutex);
    auto loaded = loadSession(id);
    if (loaded.is_err()) return Result<void>::err(loaded.error());
    if (!loaded.value()) return Result<void>::err("No such session");
    Session s = *loaded.value();
    s.note = juce::String(note).trim().toStdString();
    return saveSession(s);
}

Result<Session> SessionManager::duplicateSession(const std::string& id, const std::string& name)
{
    const std::lock_guard<std::recursive_mutex> dbLock(*m_dbMutex);
    auto loaded = loadSession(id);
    if (loaded.is_err()) return Result<Session>::err(loaded.error());
    if (!loaded.value()) return Result<Session>::err("No such session");
    const Session& original = *loaded.value();
    const auto newName = juce::String(name).trim();
    auto created = createSession(newName.isEmpty() ? original.name + " (copy)" : newName.toStdString(), original.target);
    if (created.is_err()) return created;
    Session copy = created.value();
    copy.files = original.files;
    copy.note = original.note;
    if (auto saved = saveSession(copy); saved.is_err()) return Result<Session>::err(saved.error());
    return Result<Session>::ok(std::move(copy));
}

Result<std::vector<Session>> SessionManager::listSessions(int limit)
{
    const std::lock_guard<std::recursive_mutex> dbLock(*m_dbMutex);
    if (!m_db) return Result<std::vector<Session>>::err("Database not open");
    std::vector<Session> out;
    try {
        SQLite::Statement stmt(*m_db, "SELECT data_json FROM sessions ORDER BY created_at DESC LIMIT ?");
        stmt.bind(1, limit);
        while (stmt.executeStep())
            out.push_back(sessionFromJson(stmt.getColumn(0).getString()));
        return Result<std::vector<Session>>::ok(std::move(out));
    } catch (const SQLite::Exception& e) {
        return Result<std::vector<Session>>::err(std::string("SQLite error: ") + e.what());
    }
}

namespace {

std::string escapeLike(const std::string& text)
{
    std::string escaped;
    for (char ch : text) {
        if (ch == '\\' || ch == '%' || ch == '_') escaped += '\\';
        escaped += ch;
    }
    return escaped;
}

// Session-scoped preference families, keyed "<kind>_<sessionId>" (chat
// history/decisions/pins, vocal strategy/macros, workspace recovery). Listed
// explicitly so "Delete all" also removes rows orphaned by sessions that were
// deleted before per-session cleanup existed (AUDIT-014).
constexpr const char* kSessionScopedPrefPrefixes[] = {
    "chat_history_", "chat_decisions_", "chat_pinned_", "vocal_strategy_", "vocal_macros_", "workspace_recovery_",
};

// WaveformEditorPanel stores per-source annotations under
// "annotations:<hex hashCode64 of the source path>".
std::string annotationKeyFor(const std::string& sourcePath)
{
    return "annotations:" + juce::String::toHexString(juce::String(sourcePath).hashCode64()).toStdString();
}

} // namespace

Result<void> SessionManager::deleteSession(const std::string& id)
{
    const std::lock_guard<std::recursive_mutex> dbLock(*m_dbMutex);
    if (!m_db) return Result<void>::err("Database not open");
    try {
        // The session's source paths, for its annotation prefs.
        std::vector<std::string> sourcePaths;
        {
            SQLite::Statement q(*m_db, "SELECT data_json FROM sessions WHERE id = ?");
            q.bind(1, id);
            if (q.executeStep())
                for (const auto& f : sessionFromJson(q.getColumn(0).getString()).files)
                    sourcePaths.push_back(f.path);
        }

        // One transaction: the session row (cascading analysis_results /
        // suggestion_history), its "<kind>_<sessionId>" prefs, its pending
        // outbox rows (which would otherwise re-post provenance/feedback for
        // the erased session once the service came back), and its provenance
        // events, deleted directly rather than only over best-effort HTTP
        // (AUDIT-014 / AUDIT-058).
        SQLite::Transaction tx(*m_db, SQLite::TransactionBehavior::IMMEDIATE);
        SQLite::Statement stmt(*m_db, "DELETE FROM sessions WHERE id = ?");
        stmt.bind(1, id);
        stmt.exec();
        if (!id.empty()) {
            SQLite::Statement prefs(*m_db, "DELETE FROM user_preferences WHERE key LIKE ? ESCAPE '\\'");
            prefs.bind(1, "%\\_" + escapeLike(id));
            prefs.exec();

            SQLite::Statement outbox(*m_db, "DELETE FROM recommendation_outbox WHERE json_valid(payload_json) "
                                            "AND json_extract(payload_json, '$.session_id') = ?");
            outbox.bind(1, id);
            outbox.exec();

            SQLite::Statement events(*m_db, "DELETE FROM provenance_events WHERE session_id = ?");
            events.bind(1, id);
            events.exec();

            // CE-COL-1/2 (migrations 013/014): comments and render-version
            // metadata carry no foreign key (they can arrive in a bundle
            // before the session row exists), so they go explicitly.
            for (const char* sql : {"DELETE FROM session_comments WHERE session_id = ?",
                                    "DELETE FROM render_versions WHERE session_id = ?",
                                    "DELETE FROM render_branches WHERE session_id = ?",
                                    "DELETE FROM project_tracks WHERE session_id = ?"}) { // FEAT-6 (016)
                SQLite::Statement collab(*m_db, sql);
                collab.bind(1, id);
                collab.exec();
            }
        }
        for (const auto& path : sourcePaths) {
            SQLite::Statement ann(*m_db, "DELETE FROM user_preferences WHERE key = ?");
            ann.bind(1, annotationKeyFor(path));
            ann.exec();
        }
        tx.commit();
        return Result<void>::ok();
    } catch (const SQLite::Exception& e) {
        return Result<void>::err(std::string("SQLite error: ") + e.what());
    }
}

Result<void> SessionManager::deleteAllSessions()
{
    const std::lock_guard<std::recursive_mutex> dbLock(*m_dbMutex);
    if (!m_db) return Result<void>::err("Database not open");
    try {
        SQLite::Transaction tx(*m_db, SQLite::TransactionBehavior::IMMEDIATE);
        // "<kind>_<sessionId>" keys of every session still on record, then
        // the known families (catches rows orphaned by older deletes).
        {
            SQLite::Statement ids(*m_db, "SELECT id FROM sessions");
            while (ids.executeStep()) {
                const std::string sid = ids.getColumn(0).getString();
                if (sid.empty()) continue;
                SQLite::Statement prefs(*m_db, "DELETE FROM user_preferences WHERE key LIKE ? ESCAPE '\\'");
                prefs.bind(1, "%\\_" + escapeLike(sid));
                prefs.exec();
            }
        }
        for (const char* prefix : kSessionScopedPrefPrefixes) {
            SQLite::Statement prefs(*m_db, "DELETE FROM user_preferences WHERE key LIKE ? ESCAPE '\\'");
            prefs.bind(1, escapeLike(prefix) + "%");
            prefs.exec();
        }
        m_db->exec("DELETE FROM user_preferences WHERE key LIKE 'annotations:%'");
        // Everything queued for the service describes data being erased.
        m_db->exec("DELETE FROM recommendation_outbox");
        m_db->exec("DELETE FROM provenance_events");
        m_db->exec("DELETE FROM session_comments"); // CE-COL-1 (013)
        m_db->exec("DELETE FROM render_versions");  // CE-COL-2 (014)
        m_db->exec("DELETE FROM render_branches");
        m_db->exec("DELETE FROM project_tracks"); // FEAT-6 (016)
        m_db->exec("DELETE FROM projects");
        m_db->exec("DELETE FROM sessions");
        tx.commit();
        return Result<void>::ok();
    } catch (const std::exception& e) {
        return Result<void>::err(e.what());
    }
}

Result<void> SessionManager::compactAfterErasure()
{
    const std::lock_guard<std::recursive_mutex> dbLock(*m_dbMutex);
    if (!m_db) return Result<void>::err("Database not open");
    if (m_readOnly) return Result<void>::ok();
    // secure_delete (set at open) zeroes the freed pages of THIS connection's
    // deletes; VACUUM rebuilds the file so free pages written by other
    // connections go too, and a TRUNCATE checkpoint drops the WAL, which still
    // holds the pre-delete page images (AUDIT-014). Both need the other
    // connections idle; a busy store is retried by the next erasure or the
    // periodic checkpoint, so the failure is reported, not fatal.
    try {
        m_db->exec("VACUUM");
        m_db->exec("PRAGMA wal_checkpoint(TRUNCATE)");
        return Result<void>::ok();
    } catch (const std::exception& e) {
        juce::Logger::writeToLog(juce::String("SessionManager: post-erasure compaction incomplete - ") + e.what());
        return Result<void>::err(std::string("SessionManager::compactAfterErasure: ") + e.what());
    }
}

Result<void> SessionManager::saveAnalysisResult(const AnalysisResult& r, const std::string& sessionId,
                                                const std::string& fileId, float readinessScore,
                                                const std::string& sourcePath)
{
    const std::lock_guard<std::recursive_mutex> dbLock(*m_dbMutex);
    if (!m_db) return Result<void>::err("Database not open");
    juce::DynamicObject::Ptr d = new juce::DynamicObject();
    d->setProperty("lufs_integrated", r.loudness.lufs_integrated);
    d->setProperty("lra", r.loudness.lra);
    d->setProperty("true_peak_dbtp", r.loudness.true_peak_dbtp);
    d->setProperty("crest_factor_db", r.dynamics.crest_factor_db);
    d->setProperty("dynamic_range_db", r.dynamics.dynamic_range_db);
    d->setProperty("stereo_width", r.stereo.width);
    d->setProperty("stereo_correlation", r.stereo.correlation);
    d->setProperty("channel_layout", juce::String(r.channel_layout.layout_name));
    d->setProperty("channel_count", r.channel_layout.channel_count);
    d->setProperty("lfe_excluded", r.channel_layout.lfe_excluded);
    d->setProperty("bpm", r.python_data.bpm_detected);
    d->setProperty("bpm_confidence", r.python_data.bpm_confidence);
    d->setProperty("pitch_key", juce::String(r.python_data.pitch_key));
    d->setProperty("genre_detected", juce::String(r.python_data.genre_detected));
    d->setProperty("energy_detected", r.python_data.energy_detected);
    d->setProperty("memorability_score", r.python_data.memorability.score);
    d->setProperty("hook_strength", r.python_data.hooks.hook_strength);
    d->setProperty("skewness", r.python_data.skewness);
    d->setProperty("peak_asymmetry_ratio", r.python_data.peak_asymmetry_ratio);
    d->setProperty("is_asymmetric", r.python_data.is_asymmetric);
    d->setProperty("time_signature", juce::String(r.python_data.time_signature));
    d->setProperty("meter_confidence", r.python_data.meter_confidence);
    d->setProperty("camelot", juce::String(r.python_data.camelot));
    d->setProperty("circle_of_fifths_index", r.python_data.circle_of_fifths_index);
    d->setProperty("relative_key", juce::String(r.python_data.relative_key));
    d->setProperty("parallel_key", juce::String(r.python_data.parallel_key));
    d->setProperty("recommendation_count", (int)r.recommendations.size());
    auto measured = analysis_json::measuredToVar(r); // WF-8
    if (!sourcePath.empty())                         // PERF-3: reusable on reopen
        if (auto* m = measured.getDynamicObject())
            m->setProperty("source", analysis_json::sourceStamp(juce::File(juce::String(sourcePath))));
    d->setProperty("measured", measured);
    const std::string dataJson = varToJsonString(juce::var(d.get()));

    const std::string rowId = r.id.empty() ? generateId() : r.id;
    const std::string computedAt = r.computed_at.empty() ? utcNow() : r.computed_at;

    try {
        SQLite::Statement stmt(*m_db, "INSERT OR REPLACE INTO analysis_results "
                                      "(id, session_id, file_id, computed_at, lufs_integrated, lra, true_peak_dbtp, "
                                      " dynamic_range_db, has_clipping, phase_ok, readiness_score, data_json) "
                                      "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
        stmt.bind(1, rowId);
        stmt.bind(2, sessionId);
        stmt.bind(3, fileId);
        stmt.bind(4, computedAt);
        stmt.bind(5, static_cast<double>(r.loudness.lufs_integrated));
        stmt.bind(6, static_cast<double>(r.loudness.lra));
        stmt.bind(7, static_cast<double>(r.loudness.true_peak_dbtp));
        stmt.bind(8, static_cast<double>(r.dynamics.dynamic_range_db));
        stmt.bind(9, r.artifacts.has_clipping ? 1 : 0);
        stmt.bind(10, r.phase.has_polarity_issue ? 0 : 1);
        if (readinessScore >= 0.0f)
            stmt.bind(11, static_cast<double>(readinessScore));
        else
            stmt.bind(11);
        stmt.bind(12, dataJson);
        stmt.exec();
        return Result<void>::ok();
    } catch (const SQLite::Exception& e) {
        return Result<void>::err(std::string("SessionManager::saveAnalysisResult: ") + e.what());
    } catch (const std::exception& e) {
        return Result<void>::err(std::string("SessionManager::saveAnalysisResult: ") + e.what());
    }
}

Result<std::optional<AnalysisResult>> SessionManager::loadLatestMeasured(const std::string& sessionId,
                                                                        const std::string& fileId)
{
    const std::lock_guard<std::recursive_mutex> dbLock(*m_dbMutex);
    if (!m_db) return Result<std::optional<AnalysisResult>>::err("Database not open");
    try {
        SQLite::Statement stmt(*m_db, "SELECT id, computed_at, data_json FROM analysis_results "
                                      "WHERE session_id = ? AND file_id = ? ORDER BY computed_at DESC LIMIT 1");
        stmt.bind(1, sessionId);
        stmt.bind(2, fileId);
        if (!stmt.executeStep()) return Result<std::optional<AnalysisResult>>::ok(std::nullopt);
        AnalysisResult r;
        r.id = stmt.getColumn(0).getString();
        r.computed_at = stmt.getColumn(1).getString();
        r.file_id = fileId;
        const auto data = juce::JSON::parse(juce::String::fromUTF8(stmt.getColumn(2).getText()));
        if (!analysis_json::measuredFromVar(data["measured"], r))
            return Result<std::optional<AnalysisResult>>::ok(std::nullopt);
        return Result<std::optional<AnalysisResult>>::ok(std::move(r));
    } catch (const std::exception& e) {
        return Result<std::optional<AnalysisResult>>::err(std::string("SessionManager::loadLatestMeasured: ") + e.what());
    }
}

Result<std::optional<AnalysisResult>> SessionManager::loadReusableNative(const std::string& sessionId,
                                                                        const std::string& fileId,
                                                                        const std::string& sourcePath)
{
    using Out = Result<std::optional<AnalysisResult>>;
    const std::lock_guard<std::recursive_mutex> dbLock(*m_dbMutex);
    if (!m_db) return Out::err("Database not open");
    try {
        SQLite::Statement stmt(*m_db, "SELECT data_json FROM analysis_results "
                                      "WHERE session_id = ? AND file_id = ? ORDER BY computed_at DESC LIMIT 1");
        stmt.bind(1, sessionId);
        stmt.bind(2, fileId);
        if (!stmt.executeStep()) return Out::ok(std::nullopt);
        const auto data = juce::JSON::parse(juce::String::fromUTF8(stmt.getColumn(0).getText()));
        const auto measured = data["measured"];
        if (!analysis_json::sourceMatches(measured, juce::File(juce::String(sourcePath)))) return Out::ok(std::nullopt);
        AnalysisResult r;
        r.file_id = fileId;
        if (!analysis_json::measuredFromVar(measured, r)) return Out::ok(std::nullopt);
        return Out::ok(std::move(r));
    } catch (const std::exception& e) {
        return Out::err(std::string("SessionManager::loadReusableNative: ") + e.what());
    }
}

// ---- Projects and album groups (FEAT-6) ----

namespace {
std::vector<std::string> projectTrackIds(SQLite::Database& db, const std::string& projectId)
{
    std::vector<std::string> ids;
    SQLite::Statement q(db, "SELECT session_id FROM project_tracks WHERE project_id = ? ORDER BY position, rowid");
    q.bind(1, projectId);
    while (q.executeStep())
        ids.push_back(q.getColumn(0).getString());
    return ids;
}
} // namespace

Result<Project> SessionManager::createProject(const std::string& name, const std::string& kind)
{
    const std::lock_guard<std::recursive_mutex> dbLock(*m_dbMutex);
    if (!m_db) return Result<Project>::err("Database not open");
    const auto trimmed = juce::String(name).trim().toStdString();
    if (trimmed.empty()) return Result<Project>::err("A project needs a name");
    Project p;
    p.id = generateId();
    p.name = trimmed;
    p.kind = kind.empty() ? "album" : kind;
    p.created_at = utcNow();
    p.updated_at = p.created_at;
    try {
        SQLite::Statement st(*m_db,
                             "INSERT INTO projects (id, name, kind, created_at, updated_at) VALUES (?, ?, ?, ?, ?)");
        st.bind(1, p.id);
        st.bind(2, p.name);
        st.bind(3, p.kind);
        st.bind(4, p.created_at);
        st.bind(5, p.updated_at);
        st.exec();
        return Result<Project>::ok(std::move(p));
    } catch (const SQLite::Exception& e) {
        return Result<Project>::err(std::string("SQLite error: ") + e.what());
    }
}

Result<std::optional<Project>> SessionManager::loadProject(const std::string& id)
{
    const std::lock_guard<std::recursive_mutex> dbLock(*m_dbMutex);
    using Out = Result<std::optional<Project>>;
    if (!m_db) return Out::err("Database not open");
    try {
        SQLite::Statement q(*m_db, "SELECT id, name, kind, created_at, updated_at FROM projects WHERE id = ?");
        q.bind(1, id);
        if (!q.executeStep()) return Out::ok(std::nullopt);
        Project p;
        p.id = q.getColumn(0).getString();
        p.name = q.getColumn(1).getString();
        p.kind = q.getColumn(2).getString();
        p.created_at = q.getColumn(3).getString();
        p.updated_at = q.getColumn(4).getString();
        p.session_ids = projectTrackIds(*m_db, p.id);
        return Out::ok(std::move(p));
    } catch (const SQLite::Exception& e) {
        return Out::err(std::string("SQLite error: ") + e.what());
    }
}

Result<std::vector<Project>> SessionManager::listProjects()
{
    const std::lock_guard<std::recursive_mutex> dbLock(*m_dbMutex);
    using Out = Result<std::vector<Project>>;
    if (!m_db) return Out::err("Database not open");
    try {
        std::vector<std::string> ids;
        SQLite::Statement q(*m_db, "SELECT id FROM projects ORDER BY created_at DESC");
        while (q.executeStep())
            ids.push_back(q.getColumn(0).getString());
        std::vector<Project> out;
        for (const auto& id : ids) {
            auto p = loadProject(id);
            if (p.is_err()) return Out::err(p.error());
            if (p.value()) out.push_back(std::move(*p.value()));
        }
        return Out::ok(std::move(out));
    } catch (const SQLite::Exception& e) {
        return Out::err(std::string("SQLite error: ") + e.what());
    }
}

Result<std::optional<Project>> SessionManager::projectOfSession(const std::string& sessionId)
{
    const std::lock_guard<std::recursive_mutex> dbLock(*m_dbMutex);
    using Out = Result<std::optional<Project>>;
    if (!m_db) return Out::err("Database not open");
    try {
        std::string projectId;
        {
            SQLite::Statement q(*m_db, "SELECT project_id FROM project_tracks WHERE session_id = ?");
            q.bind(1, sessionId);
            if (!q.executeStep()) return Out::ok(std::nullopt);
            projectId = q.getColumn(0).getString();
        }
        return loadProject(projectId);
    } catch (const SQLite::Exception& e) {
        return Out::err(std::string("SQLite error: ") + e.what());
    }
}

Result<void> SessionManager::addSessionToProject(const std::string& projectId, const std::string& sessionId)
{
    const std::lock_guard<std::recursive_mutex> dbLock(*m_dbMutex);
    if (!m_db) return Result<void>::err("Database not open");
    try {
        SQLite::Transaction tx(*m_db, SQLite::TransactionBehavior::IMMEDIATE);
        {
            SQLite::Statement exists(*m_db, "SELECT 1 FROM projects WHERE id = ?");
            exists.bind(1, projectId);
            if (!exists.executeStep()) return Result<void>::err("No such project");
        }
        SQLite::Statement drop(*m_db, "DELETE FROM project_tracks WHERE session_id = ?");
        drop.bind(1, sessionId);
        drop.exec();
        SQLite::Statement ins(*m_db, "INSERT INTO project_tracks (project_id, session_id, position) "
                                     "VALUES (?, ?, (SELECT COALESCE(MAX(position) + 1, 0) FROM project_tracks "
                                     "WHERE project_id = ?))");
        ins.bind(1, projectId);
        ins.bind(2, sessionId);
        ins.bind(3, projectId);
        ins.exec();
        SQLite::Statement touch(*m_db, "UPDATE projects SET updated_at = ? WHERE id = ?");
        touch.bind(1, utcNow());
        touch.bind(2, projectId);
        touch.exec();
        tx.commit();
        return Result<void>::ok();
    } catch (const SQLite::Exception& e) {
        return Result<void>::err(std::string("SQLite error: ") + e.what());
    }
}

Result<void> SessionManager::removeSessionFromProject(const std::string& sessionId)
{
    const std::lock_guard<std::recursive_mutex> dbLock(*m_dbMutex);
    if (!m_db) return Result<void>::err("Database not open");
    try {
        SQLite::Statement drop(*m_db, "DELETE FROM project_tracks WHERE session_id = ?");
        drop.bind(1, sessionId);
        drop.exec();
        return Result<void>::ok();
    } catch (const SQLite::Exception& e) {
        return Result<void>::err(std::string("SQLite error: ") + e.what());
    }
}

Result<void> SessionManager::setProjectOrder(const std::string& projectId, const std::vector<std::string>& sessionIds)
{
    const std::lock_guard<std::recursive_mutex> dbLock(*m_dbMutex);
    if (!m_db) return Result<void>::err("Database not open");
    try {
        SQLite::Transaction tx(*m_db, SQLite::TransactionBehavior::IMMEDIATE);
        int position = 0;
        for (const auto& sid : sessionIds) {
            SQLite::Statement up(*m_db,
                                 "UPDATE project_tracks SET position = ? WHERE project_id = ? AND session_id = ?");
            up.bind(1, position);
            up.bind(2, projectId);
            up.bind(3, sid);
            if (up.exec() > 0) ++position;
        }
        tx.commit();
        return Result<void>::ok();
    } catch (const SQLite::Exception& e) {
        return Result<void>::err(std::string("SQLite error: ") + e.what());
    }
}

Result<void> SessionManager::renameProject(const std::string& id, const std::string& name)
{
    const std::lock_guard<std::recursive_mutex> dbLock(*m_dbMutex);
    if (!m_db) return Result<void>::err("Database not open");
    const auto trimmed = juce::String(name).trim().toStdString();
    if (trimmed.empty()) return Result<void>::err("A project needs a name");
    try {
        SQLite::Statement up(*m_db, "UPDATE projects SET name = ?, updated_at = ? WHERE id = ?");
        up.bind(1, trimmed);
        up.bind(2, utcNow());
        up.bind(3, id);
        if (up.exec() == 0) return Result<void>::err("No such project");
        return Result<void>::ok();
    } catch (const SQLite::Exception& e) {
        return Result<void>::err(std::string("SQLite error: ") + e.what());
    }
}

Result<void> SessionManager::deleteProject(const std::string& id)
{
    const std::lock_guard<std::recursive_mutex> dbLock(*m_dbMutex);
    if (!m_db) return Result<void>::err("Database not open");
    try {
        SQLite::Transaction tx(*m_db, SQLite::TransactionBehavior::IMMEDIATE);
        for (const char* sql :
             {"DELETE FROM project_tracks WHERE project_id = ?", "DELETE FROM projects WHERE id = ?"}) {
            SQLite::Statement st(*m_db, sql);
            st.bind(1, id);
            st.exec();
        }
        tx.commit();
        return Result<void>::ok();
    } catch (const SQLite::Exception& e) {
        return Result<void>::err(std::string("SQLite error: ") + e.what());
    }
}

Result<std::optional<SessionManager::AnalysisSummary>>
SessionManager::latestAnalysisSummary(const std::string& sessionId)
{
    const std::lock_guard<std::recursive_mutex> dbLock(*m_dbMutex);
    using Out = Result<std::optional<AnalysisSummary>>;
    if (!m_db) return Out::err("Database not open");
    try {
        SQLite::Statement q(*m_db,
                            "SELECT ar.computed_at, ar.lufs_integrated, ar.true_peak_dbtp, ar.dynamic_range_db, "
                            "       ar.has_clipping, ar.phase_ok, ar.readiness_score, s.name "
                            "FROM analysis_results ar INNER JOIN sessions s ON s.id = ar.session_id "
                            "WHERE ar.session_id = ? ORDER BY ar.computed_at DESC LIMIT 1");
        q.bind(1, sessionId);
        if (!q.executeStep()) return Out::ok(std::nullopt);
        AnalysisSummary sum;
        sum.session_id = sessionId;
        sum.computed_at = q.getColumn(0).getString();
        sum.lufs_integrated = static_cast<float>(q.getColumn(1).getDouble());
        sum.true_peak_dbtp = static_cast<float>(q.getColumn(2).getDouble());
        sum.dynamic_range_db = static_cast<float>(q.getColumn(3).getDouble());
        sum.has_clipping = q.getColumn(4).getInt() != 0;
        sum.phase_ok = q.getColumn(5).isNull() ? true : q.getColumn(5).getInt() != 0;
        sum.readiness_score = q.getColumn(6).isNull() ? -1.0f : static_cast<float>(q.getColumn(6).getDouble());
        sum.session_name = q.getColumn(7).getString();
        return Out::ok(std::move(sum));
    } catch (const SQLite::Exception& e) {
        return Out::err(std::string("SQLite error: ") + e.what());
    }
}

Result<std::vector<SessionManager::AnalysisSummary>> SessionManager::listRecentAnalysisSummaries(int limit)
{
    const std::lock_guard<std::recursive_mutex> dbLock(*m_dbMutex);
    std::vector<AnalysisSummary> out;
    try {
        // One row per session: the most recently computed_at analysis for
        // each of the last `limit` sessions (by session creation order).
        SQLite::Statement stmt(
            *m_db, "SELECT ar.session_id, ar.computed_at, ar.lufs_integrated, ar.data_json, s.name, "
                   "       ar.true_peak_dbtp, ar.dynamic_range_db, ar.has_clipping, ar.phase_ok, ar.readiness_score "
                   "FROM analysis_results ar "
                   "INNER JOIN ("
                   "    SELECT session_id, MAX(computed_at) AS latest "
                   "    FROM analysis_results GROUP BY session_id"
                   ") latest_ar ON ar.session_id = latest_ar.session_id AND ar.computed_at = latest_ar.latest "
                   "INNER JOIN sessions s ON s.id = ar.session_id "
                   "ORDER BY s.created_at DESC LIMIT ?");
        stmt.bind(1, limit);
        while (stmt.executeStep()) {
            AnalysisSummary sum;
            sum.session_id = stmt.getColumn(0).getString();
            sum.computed_at = stmt.getColumn(1).getString();
            sum.lufs_integrated = static_cast<float>(stmt.getColumn(2).getDouble());
            sum.session_name = stmt.getColumn(4).getString();
            // Columns added by migration 6; NULL for rows saved before it existed
            // (SQLiteCpp's getDouble()/getInt() return 0 for NULL, which is an
            // honest "no data" default here - not a fabricated real reading).
            sum.true_peak_dbtp = static_cast<float>(stmt.getColumn(5).getDouble());
            sum.dynamic_range_db = static_cast<float>(stmt.getColumn(6).getDouble());
            sum.has_clipping = stmt.getColumn(7).getInt() != 0;
            sum.phase_ok = stmt.getColumn(8).isNull() ? true : stmt.getColumn(8).getInt() != 0;
            sum.readiness_score =
                stmt.getColumn(9).isNull() ? -1.0f : static_cast<float>(stmt.getColumn(9).getDouble());

            auto data = parseJsonString(stmt.getColumn(3).getString());
            if (auto* obj = data.getDynamicObject()) {
                sum.energy_detected = static_cast<float>(obj->getProperty("energy_detected"));
                sum.hook_strength = static_cast<float>(obj->getProperty("hook_strength"));
                sum.memorability_score = static_cast<float>(obj->getProperty("memorability_score"));
                sum.bpm = static_cast<float>(obj->getProperty("bpm"));
                sum.genre_detected = obj->getProperty("genre_detected").toString().toStdString();
            }
            out.push_back(std::move(sum));
        }
        return Result<std::vector<AnalysisSummary>>::ok(std::move(out));
    } catch (const SQLite::Exception& e) {
        return Result<std::vector<AnalysisSummary>>::err(std::string("SQLite error: ") + e.what());
    }
}

Result<void> SessionManager::setUserPreference(const std::string& key, const std::string& valueJson)
{
    const std::lock_guard<std::recursive_mutex> dbLock(*m_dbMutex);
    if (!m_db) return Result<void>::err("Database not open");
    try {
        SQLite::Statement query(
            *m_db, "INSERT INTO user_preferences (key, value_json, updated_at) VALUES (?, ?, ?) "
                   "ON CONFLICT(key) DO UPDATE SET value_json = excluded.value_json, updated_at = excluded.updated_at");
        query.bind(1, key);
        query.bind(2, valueJson);
        query.bind(3, utcNow());
        query.exec();
        return Result<void>::ok();
    } catch (const std::exception& e) {
        return Result<void>::err(e.what());
    }
}

Result<std::string> SessionManager::getUserPreference(const std::string& key)
{
    const std::lock_guard<std::recursive_mutex> dbLock(*m_dbMutex);
    if (!m_db) return Result<std::string>::err("Database not open");
    try {
        SQLite::Statement query(*m_db, "SELECT value_json FROM user_preferences WHERE key = ?");
        query.bind(1, key);
        if (query.executeStep()) {
            return Result<std::string>::ok(query.getColumn(0).getText());
        }
        return Result<std::string>::err("Key not found");
    } catch (const std::exception& e) {
        return Result<std::string>::err(e.what());
    }
}

std::vector<std::string> SessionManager::listUserPreferenceKeysWithPrefix(const std::string& prefix)
{
    const std::lock_guard<std::recursive_mutex> dbLock(*m_dbMutex);
    std::vector<std::string> keys;
    if (!m_db) return keys;
    try {
        // ESCAPE so '_' and '%' in the prefix (e.g. "dashboard_layout_") are
        // literal - unescaped, '_' matched any character.
        std::string escaped;
        for (char ch : prefix) {
            if (ch == '\\' || ch == '%' || ch == '_') escaped += '\\';
            escaped += ch;
        }
        SQLite::Statement query(*m_db,
                                "SELECT key FROM user_preferences WHERE key LIKE ? ESCAPE '\\' ORDER BY key");
        query.bind(1, escaped + "%");
        while (query.executeStep())
            keys.emplace_back(query.getColumn(0).getText());
    } catch (const std::exception&) {}
    return keys;
}

Result<void> SessionManager::saveScannedPlugins(const std::vector<mixerchain::ScannedPlugin>& plugins)
{
    const std::lock_guard<std::recursive_mutex> dbLock(*m_dbMutex);
    if (!m_db) return Result<void>::err("Database not open");
    try {
        SQLite::Transaction tx(*m_db);
        m_db->exec("DELETE FROM scanned_plugins");

        SQLite::Statement stmt(
            *m_db, "INSERT INTO scanned_plugins (id, name, manufacturer, category, format, file_path, updated_at) "
                   "VALUES (?, ?, ?, ?, ?, ?, ?)");

        std::string now = utcNow();
        for (const auto& p : plugins) {
            stmt.bind(1, p.id);
            stmt.bind(2, p.name);
            stmt.bind(3, p.manufacturer);
            stmt.bind(4, p.category);
            stmt.bind(5, p.format);
            stmt.bind(6, p.file_path);
            stmt.bind(7, now);
            stmt.exec();
            stmt.reset();
        }

        tx.commit();
        return Result<void>::ok();
    } catch (const std::exception& e) {
        return Result<void>::err(e.what());
    }
}

Result<std::vector<mixerchain::ScannedPlugin>> SessionManager::listScannedPlugins()
{
    const std::lock_guard<std::recursive_mutex> dbLock(*m_dbMutex);
    if (!m_db) return Result<std::vector<mixerchain::ScannedPlugin>>::err("Database not open");
    try {
        std::vector<mixerchain::ScannedPlugin> out;
        SQLite::Statement query(*m_db,
                                "SELECT id, name, manufacturer, category, format, file_path FROM scanned_plugins");

        while (query.executeStep()) {
            mixerchain::ScannedPlugin p;
            p.id = query.getColumn(0).getText();
            p.name = query.getColumn(1).getText();
            p.manufacturer = query.getColumn(2).getText();
            p.category = query.getColumn(3).getText();
            p.format = query.getColumn(4).getText();
            p.file_path = query.getColumn(5).getText();
            out.push_back(std::move(p));
        }
        return Result<std::vector<mixerchain::ScannedPlugin>>::ok(out);
    } catch (const std::exception& e) {
        return Result<std::vector<mixerchain::ScannedPlugin>>::err(e.what());
    }
}

// ---------------------------------------------------------------------------
// Transactional outbox (audit ARCH-03)
// ---------------------------------------------------------------------------

Result<void> SessionManager::enqueueOutboxEvent(const std::string& idempotencyKey, const std::string& kind,
                                                 const std::string& payloadJson)
{
    const std::lock_guard<std::recursive_mutex> dbLock(*m_dbMutex);
    if (!m_db) return Result<void>::err("Database not open");
    try {
        // INSERT OR IGNORE on the UNIQUE idempotency_key: a caller retrying
        // enqueue() after a crash between insert and its own commit doesn't
        // produce a second row for the same logical event.
        SQLite::Statement stmt(
            *m_db, "INSERT OR IGNORE INTO recommendation_outbox "
                   "(idempotency_key, kind, payload_json, status, attempts, created_at) "
                   "VALUES (?, ?, ?, 'pending', 0, ?)");
        stmt.bind(1, idempotencyKey);
        stmt.bind(2, kind);
        stmt.bind(3, payloadJson);
        stmt.bind(4, utcNow());
        stmt.exec();
        return Result<void>::ok();
    } catch (const std::exception& e) {
        return Result<void>::err(e.what());
    }
}

Result<void> SessionManager::enqueueOutboxEvents(const std::vector<OutboxEnqueue>& events)
{
    const std::lock_guard<std::recursive_mutex> dbLock(*m_dbMutex);
    if (!m_db) return Result<void>::err("Database not open");
    try {
        // All-or-nothing: a feedback mirror and its provenance event are one
        // logical action, so a crash can't leave just one of them queued.
        SQLite::Transaction tx(*m_db);
        for (const auto& e : events) {
            auto r = enqueueOutboxEvent(e.idempotencyKey, e.kind, e.payloadJson);
            if (!r.is_ok()) return r; // tx rolls back
        }
        tx.commit();
        return Result<void>::ok();
    } catch (const std::exception& e) {
        return Result<void>::err(e.what());
    }
}

namespace {
constexpr const char* kOutboxColumns = "SELECT id, idempotency_key, kind, payload_json, status, attempts, created_at, "
                                       "COALESCE(last_error, '') FROM recommendation_outbox ";
// Dead-lettered rows keep status 'failed' (the table's CHECK constraint has no
// fourth state) and are told apart by this last_error prefix.
constexpr const char* kNotDeadLettered = "COALESCE(last_error, '') NOT LIKE 'dead-letter:%' ";

SessionManager::OutboxEntry readOutboxRow(SQLite::Statement& query)
{
    SessionManager::OutboxEntry e;
    e.id = query.getColumn(0).getInt64();
    e.idempotencyKey = query.getColumn(1).getText();
    e.kind = query.getColumn(2).getText();
    e.payloadJson = query.getColumn(3).getText();
    e.status = query.getColumn(4).getText();
    e.attempts = query.getColumn(5).getInt();
    e.createdAt = query.getColumn(6).getText();
    e.lastError = query.getColumn(7).getText();
    return e;
}
} // namespace

std::vector<SessionManager::OutboxEntry> SessionManager::pendingOutboxEntries(int limit)
{
    const std::lock_guard<std::recursive_mutex> dbLock(*m_dbMutex);
    std::vector<OutboxEntry> out;
    if (!m_db) return out;
    try {
        SQLite::Statement query(*m_db, std::string(kOutboxColumns) + "WHERE status != 'delivered' AND " +
                                           kNotDeadLettered + "ORDER BY id ASC LIMIT ?");
        query.bind(1, limit);
        while (query.executeStep())
            out.push_back(readOutboxRow(query));
    } catch (const std::exception&) {}
    return out;
}

std::vector<SessionManager::OutboxEntry> SessionManager::dueOutboxEntries(int limit)
{
    const std::lock_guard<std::recursive_mutex> dbLock(*m_dbMutex);
    std::vector<OutboxEntry> out;
    if (!m_db) return out;
    try {
        // AUDIT-056: the backoff is applied in SQL, before LIMIT, so a block of
        // rows that keep failing can't fill every batch and starve newer ones.
        // Delay after N failed attempts: 2 << N seconds (4 s, 8 s, ...), capped
        // at one hour.
        SQLite::Statement query(
            *m_db, std::string(kOutboxColumns) + "WHERE status != 'delivered' AND " + kNotDeadLettered +
                       "AND (status = 'pending' OR last_attempt_at IS NULL OR julianday(last_attempt_at) "
                       "+ MIN(3600, 2 << MIN(attempts, 11)) / 86400.0 <= julianday('now')) "
                       "ORDER BY id ASC LIMIT ?");
        query.bind(1, limit);
        while (query.executeStep())
            out.push_back(readOutboxRow(query));
    } catch (const std::exception&) {}
    return out;
}

bool SessionManager::isOutboxEntryUndelivered(long long id)
{
    const std::lock_guard<std::recursive_mutex> dbLock(*m_dbMutex);
    if (!m_db) return false;
    try {
        SQLite::Statement query(*m_db, "SELECT 1 FROM recommendation_outbox WHERE id = ? AND status != 'delivered'");
        query.bind(1, id);
        return query.executeStep();
    } catch (const std::exception&) {
        return false;
    }
}

Result<void> SessionManager::markOutboxDelivered(long long id)
{
    const std::lock_guard<std::recursive_mutex> dbLock(*m_dbMutex);
    if (!m_db) return Result<void>::err("Database not open");
    try {
        SQLite::Statement stmt(*m_db, "UPDATE recommendation_outbox SET status = 'delivered', "
                                      "last_attempt_at = ? WHERE id = ?");
        stmt.bind(1, utcNow());
        stmt.bind(2, id);
        stmt.exec();
        return Result<void>::ok();
    } catch (const std::exception& e) {
        return Result<void>::err(e.what());
    }
}

Result<void> SessionManager::markOutboxFailed(long long id, const std::string& error)
{
    const std::lock_guard<std::recursive_mutex> dbLock(*m_dbMutex);
    if (!m_db) return Result<void>::err("Database not open");
    try {
        SQLite::Statement stmt(*m_db, "UPDATE recommendation_outbox SET status = 'failed', "
                                      "attempts = attempts + 1, last_attempt_at = ?, last_error = ? WHERE id = ?");
        stmt.bind(1, utcNow());
        stmt.bind(2, error);
        stmt.bind(3, id);
        stmt.exec();
        return Result<void>::ok();
    } catch (const std::exception& e) {
        return Result<void>::err(e.what());
    }
}

Result<void> SessionManager::markOutboxDeadLettered(long long id, const std::string& error)
{
    return markOutboxFailed(id, std::string(kOutboxDeadLetterPrefix) + error);
}

int SessionManager::undeliveredOutboxCount()
{
    const std::lock_guard<std::recursive_mutex> dbLock(*m_dbMutex);
    if (!m_db) return 0;
    try {
        SQLite::Statement query(*m_db, std::string("SELECT COUNT(*) FROM recommendation_outbox "
                                                   "WHERE status != 'delivered' AND ") + kNotDeadLettered);
        if (query.executeStep()) return query.getColumn(0).getInt();
    } catch (const std::exception&) {}
    return 0;
}

int SessionManager::deadLetteredOutboxCount()
{
    const std::lock_guard<std::recursive_mutex> dbLock(*m_dbMutex);
    if (!m_db) return 0;
    try {
        SQLite::Statement query(*m_db, "SELECT COUNT(*) FROM recommendation_outbox "
                                       "WHERE status = 'failed' AND last_error LIKE 'dead-letter:%'");
        if (query.executeStep()) return query.getColumn(0).getInt();
    } catch (const std::exception&) {}
    return 0;
}

int SessionManager::pruneDeliveredOutbox(int olderThanDays)
{
    const std::lock_guard<std::recursive_mutex> dbLock(*m_dbMutex);
    if (!m_db) return 0;
    try {
        // Delivered rows were never removed, so the table grew forever.
        SQLite::Statement stmt(*m_db, "DELETE FROM recommendation_outbox WHERE status = 'delivered' AND "
                                      "(last_attempt_at IS NULL OR julianday(last_attempt_at) <= julianday('now', ?))");
        stmt.bind(1, "-" + std::to_string(std::max(0, olderThanDays)) + " days");
        return stmt.exec();
    } catch (const std::exception&) {
        return 0;
    }
}

// ---------------------------------------------------------------------------
// Backup / export (audit DATA-02)
// ---------------------------------------------------------------------------

Result<void> SessionManager::backupTo(const std::string& destPath)
{
    const std::lock_guard<std::recursive_mutex> dbLock(*m_dbMutex);
    if (!m_db) return Result<void>::err("Database not open");
    const juce::File dest{juce::String(destPath)};
    const juce::File parent = dest.getParentDirectory();
    if (!parent.exists() || !parent.isDirectory())
        return Result<void>::err("SessionManager::backupTo: destination directory does not exist");
    if (dest.exists() && !dest.existsAsFile())
        return Result<void>::err("SessionManager::backupTo: destination is not a file");

    // Create the complete snapshot beside the destination. A same-volume
    // replacement is atomic on the supported filesystems, so an interrupted
    // backup leaves the previous known-good file untouched.
    const juce::File temp = parent.getChildFile(
        dest.getFileName() + ".tmp-" + juce::Uuid().toString());
    try {
        temp.deleteFile();
        {
            SQLite::Database tempDb(temp.getFullPathName().toStdString(), SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
            // Online backup API: consistent snapshot while the source stays open
            // and in WAL mode - no checkpoint/quiesce needed (audit DATA-02).
            SQLite::Backup backup(tempDb, *m_db);
            backup.executeStep(); // -1 default = copy all pages in one step
        } // close the replacement candidate before moving it into place

        const bool replaced = dest.existsAsFile() ? temp.replaceFileIn(dest) : temp.moveFileTo(dest);
        if (!replaced)
            return Result<void>::err("SessionManager::backupTo: could not atomically replace destination");
        return Result<void>::ok();
    } catch (const std::exception& e) {
        temp.deleteFile();
        return Result<void>::err(std::string("SessionManager::backupTo: ") + e.what());
    }
}

Result<std::string> SessionManager::exportSessionJson(const std::string& id)
{
    const std::lock_guard<std::recursive_mutex> dbLock(*m_dbMutex);
    if (!m_db) return Result<std::string>::err("Database not open");
    try {
        juce::DynamicObject::Ptr root = new juce::DynamicObject();

        SQLite::Statement s(*m_db, "SELECT id, name, created_at, updated_at, data_json FROM sessions WHERE id = ?");
        s.bind(1, id);
        if (!s.executeStep()) return Result<std::string>::err("Session not found");

        // The columns are UTF-8; juce::String(const char*) would read them
        // as Latin-1 and export a non-ASCII name as mojibake (FIX-18).
        juce::DynamicObject::Ptr session = new juce::DynamicObject();
        session->setProperty("id", juce::String::fromUTF8(s.getColumn(0).getText()));
        session->setProperty("name", juce::String::fromUTF8(s.getColumn(1).getText()));
        session->setProperty("created_at", juce::String::fromUTF8(s.getColumn(2).getText()));
        session->setProperty("updated_at", juce::String::fromUTF8(s.getColumn(3).getText()));
        session->setProperty("data", parseJsonString(s.getColumn(4).getText()));
        root->setProperty("session", juce::var(session.get()));

        juce::Array<juce::var> analyses;
        SQLite::Statement a(*m_db, "SELECT id, file_id, computed_at, data_json "
                                   "FROM analysis_results WHERE session_id = ? ORDER BY computed_at");
        a.bind(1, id);
        while (a.executeStep()) {
            juce::DynamicObject::Ptr row = new juce::DynamicObject();
            row->setProperty("id", juce::String::fromUTF8(a.getColumn(0).getText()));
            row->setProperty("file_id", juce::String::fromUTF8(a.getColumn(1).getText()));
            row->setProperty("computed_at", juce::String::fromUTF8(a.getColumn(2).getText()));
            row->setProperty("data", parseJsonString(a.getColumn(3).getText()));
            analyses.add(juce::var(row.get()));
        }
        root->setProperty("analysis_results", analyses);
        root->setProperty("export_format_version", 1);

        return Result<std::string>::ok(varToJsonString(juce::var(root.get())));
    } catch (const std::exception& e) {
        return Result<std::string>::err(std::string("SessionManager::exportSessionJson: ") + e.what());
    }
}

} // namespace khensu
