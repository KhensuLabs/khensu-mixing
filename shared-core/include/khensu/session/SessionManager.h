#pragma once
#include <mutex>
#include <khensu/session/Project.h>
#include <khensu/session/Session.h>
#include <khensu/analysis/AnalysisResult.h>
#include <khensu/common/Result.h>
#include <optional>
#include <string>
#include <vector>
#include <memory>
#include <stdexcept>

// Forward declare SQLite::Database to avoid exposing SQLiteCpp in public headers.
namespace SQLite {
class Database;
}

namespace khensu {
namespace mixerchain {
struct ScannedPlugin;
}

/// Persists and retrieves Sessions from a SQLite database.
/// Pass ":memory:" as dbPath for an in-memory database (useful in tests).
///
/// THREAD_CONTEXT(WorkerThread) - all methods do file I/O or SQLite operations.
/// Do NOT call from the AudioThread.
class SessionManager {
  public:
    /// ReadWrite (default) creates the file if needed and applies pending
    /// migrations. ReadOnly opens an existing file without creating,
    /// migrating or writing - for readers such as the VST3 plugin, which used
    /// to race the desktop app's migration runner (AUDIT-013). Throws
    /// SQLite::Exception if the file can't be opened.
    enum class OpenMode { ReadWrite, ReadOnly };

    /// Thrown by either constructor when the file was migrated by a NEWER
    /// build (schema_version above latestSchemaVersion()): this build must
    /// not write tables it doesn't understand (AUDIT-087), nor read them as
    /// if they had not moved (FIX-18). Not transient, so callers should not
    /// retry it.
    struct SchemaTooNewError : std::runtime_error {
        SchemaTooNewError(int dbVersion, int supportedVersion);
        int dbVersion;
        int supportedVersion;
    };

    explicit SessionManager(const std::string& dbPath, OpenMode mode = OpenMode::ReadWrite);
    ~SessionManager();

    // Non-copyable, movable
    SessionManager(const SessionManager&) = delete;
    SessionManager& operator=(const SessionManager&) = delete;
    SessionManager(SessionManager&&) noexcept;
    SessionManager& operator=(SessionManager&&) noexcept;

    /// Create a new session and persist it. Populates session.id and timestamps.
    Result<Session> createSession(const std::string& name, const CreativeTarget& target = {});

    /// Load a session by id (audit CQ-05: a typed result, not a side flag).
    /// ok(nullopt) = no such session; ok(session) = found; err = the read
    /// itself failed (locked/corrupt database) - "couldn't open" and "doesn't
    /// exist" are different states and callers must not conflate them.
    Result<std::optional<Session>> loadSession(const std::string& id);

    /// Persist changes to an existing session. Updates updated_at.
    Result<void> saveSession(Session& session);

    /// WF-12: renames a session (its record only). err for an empty name or
    /// no such session.
    Result<void> renameSession(const std::string& id, const std::string& name);
    /// WF-12: sets the session's version note (empty clears it).
    Result<void> setSessionNote(const std::string& id, const std::string& note);
    /// WF-12: a new session with the same files, target and note - not its
    /// analyses, renders or chat, which belong to the original.
    Result<Session> duplicateSession(const std::string& id, const std::string& name);

    /// List all sessions, newest first (audit CQ-05). ok(empty) genuinely
    /// means "no sessions yet"; a failed read is err, never an empty vector.
    // ---- Projects and album groups (FEAT-6, migration 016) ----
    /// A new, empty project.
    Result<Project> createProject(const std::string& name, const std::string& kind = "album");
    /// The project with its tracks in order; nullopt when there is none.
    Result<std::optional<Project>> loadProject(const std::string& id);
    /// Every project, newest first, each with its tracks in order.
    Result<std::vector<Project>> listProjects();
    /// The project `sessionId` belongs to, if any.
    Result<std::optional<Project>> projectOfSession(const std::string& sessionId);
    /// Appends the session as the project's last track; a session in another
    /// project moves here (one project per session).
    Result<void> addSessionToProject(const std::string& projectId, const std::string& sessionId);
    Result<void> removeSessionFromProject(const std::string& sessionId);
    /// Sets the track order to `sessionIds` (the project's own tracks only).
    Result<void> setProjectOrder(const std::string& projectId, const std::vector<std::string>& sessionIds);
    Result<void> renameProject(const std::string& id, const std::string& name);
    /// Deletes the project; its sessions stay.
    Result<void> deleteProject(const std::string& id);

    Result<std::vector<Session>> listSessions(int limit = 50);

    /// Test-only: execute arbitrary SQL against the underlying connection
    /// (e.g. DROP a table) so tests can force a real SQLite failure and
    /// verify the error path actually surfaces it, without SessionManager
    /// needing any test-mode branching in its production code paths.
    void execRawSqlForTesting(const std::string& sql);

    /// Delete a session and, in the same transaction, everything derived
    /// from it in khensu.db: analysis results / suggestion history (FK
    /// cascade), its "<kind>_<sessionId>" preferences and source-file
    /// annotations, its queued outbox rows (payload session_id) and its
    /// provenance_events (AUDIT-014 / AUDIT-058). Call compactAfterErasure()
    /// afterwards (off the message thread) to purge the freed pages and WAL.
    Result<void> deleteSession(const std::string& id);

    /// Erases every session (cascading to analysis_results/suggestion_history)
    /// plus all session-scoped content in one transaction: chat history /
    /// decisions / pins, vocal and workspace-recovery prefs, annotations, the
    /// whole recommendation outbox (so nothing re-posts erased data to the
    /// service) and every provenance event - the khensu.db half of "Delete
    /// all Khensu data" (audit DATA-02, AUDIT-014). App-level settings
    /// (theme, layouts, calibration) and scanned_plugins stay. Irreversible;
    /// the caller confirms with the user first, then calls
    /// compactAfterErasure().
    Result<void> deleteAllSessions();

    /// After an erasure: VACUUM (drops free pages, including other
    /// connections') and wal_checkpoint(TRUNCATE) (drops the WAL's pre-delete
    /// page images). Needs the other connections idle; err when the store was
    /// busy (the rows themselves are already deleted). Slow on a large DB -
    /// WorkerThread only.
    Result<void> compactAfterErasure();

    /// Upsert an analysis result for a session. Replaces any prior row with the same id.
    /// `readinessScore` is optional (-1 = not computed): SessionManager is a pure
    /// persistence layer and doesn't depend on RecommendationEngine/MixReportBuilder,
    /// so the caller (which already has genre/target/weight-profile context) computes
    /// it and passes it in, the same way DashboardScreen computes it for the readiness tile.
    /// Call from WorkerThread only - performs synchronous SQLite I/O.
    /// Returns an error when the result was not durably recorded. Callers must
    /// surface this rather than presenting an analysis as saved when SQLite is
    /// locked, full, or otherwise unavailable.
    /// `sourcePath`, when given, stamps the stored native pass with the file's
    /// size and modification time so a reopen can reuse it (PERF-3).
    Result<void> saveAnalysisResult(const AnalysisResult& r, const std::string& sessionId,
                                    const std::string& fileId, float readinessScore = -1.0f,
                                    const std::string& sourcePath = {});
    /// PERF-3: the stored native pass of `fileId`, only while its stamp still
    /// matches the file at `sourcePath` (same bytes, same native analysers).
    Result<std::optional<AnalysisResult>> loadReusableNative(const std::string& sessionId,
                                                             const std::string& fileId,
                                                             const std::string& sourcePath);

    /// Compact snapshot of an analysis_results row, for cross-session trend display.
    struct AnalysisSummary {
        std::string session_id;
        std::string session_name; // e.g. often used to note the client/project
        std::string computed_at;
        float lufs_integrated = 0.0f;
        float energy_detected = 0.0f;
        float hook_strength = 0.0f;
        float memorability_score = 0.0f;
        float bpm = 0.0f;
        std::string genre_detected;
        // Added for the "Compare to last session" tile/trend view - true peak,
        // DR, clipping and polarity were already computed per-analysis but not
        // surfaced past the raw data_json blob; readiness needs the caller to
        // pass it into saveAnalysisResult (see above) since it isn't derivable
        // from the AnalysisResult alone.
        float true_peak_dbtp = 0.0f;
        float dynamic_range_db = 0.0f;
        bool has_clipping = false;
        bool phase_ok = true;
        float readiness_score = -1.0f; // -1 = not available for this row
    };

    /// Returns the most recent analysis_results row for each of the last `limit`
    /// sessions that have one, newest first. Call from WorkerThread only.
    /// ok(empty) genuinely means "no analysis yet"; a failed read is err
    /// (audit CQ-05: typed result, not a side flag).
    Result<std::vector<AnalysisSummary>> listRecentAnalysisSummaries(int limit = 10);
    /// The newest analysis summary of one session (the readiness board).
    Result<std::optional<AnalysisSummary>> latestAnalysisSummary(const std::string& sessionId);
    /// WF-8: the newest stored analysis of `fileId` in `sessionId` with its
    /// measured block (AnalysisResultJson) restored - std::nullopt when there
    /// is none, or it predates the block. Lets the plugin grade what the
    /// desktop measured instead of analysing the file again.
    Result<std::optional<AnalysisResult>> loadLatestMeasured(const std::string& sessionId, const std::string& fileId);

    /// Set a key-value user preference JSON string.
    Result<void> setUserPreference(const std::string& key, const std::string& valueJson);

    /// Get a key-value user preference JSON string.
    Result<std::string> getUserPreference(const std::string& key);

    /// Lists all user_preferences keys starting with `prefix` (e.g. saved
    /// Dashboard Layout / workspace-profile names), sorted alphabetically.
    /// Empty on failure or no matches - best-effort, not a Result<> since
    /// callers just want a list to populate a picker.
    std::vector<std::string> listUserPreferenceKeysWithPrefix(const std::string& prefix);

    /// Save scanned plugins to the database, replacing existing entries.
    Result<void> saveScannedPlugins(const std::vector<mixerchain::ScannedPlugin>& plugins);

    /// Load the list of scanned plugins from the database.
    Result<std::vector<mixerchain::ScannedPlugin>> listScannedPlugins();

    bool isOpen() const noexcept;
    bool isReadOnly() const noexcept;

    /// Highest migration version this build knows (the last entry of
    /// migrations()).
    static int latestSchemaVersion();

    /// For a connection that does not run the migration chain (a read-only
    /// SessionManager, a guest FeedbackManager): throws SchemaTooNewError
    /// when `db` records a schema version above latestSchemaVersion(). A
    /// file with no schema_version table predates the chain and is allowed.
    /// Reads only.
    static void throwIfSchemaTooNew(SQLite::Database& db);

    /// One numbered schema migration: `version`'s SQL, applied once and
    /// recorded in `schema_version`. Exposed (not private) so both
    /// runMigrations() and a test can read the exact same list — the
    /// checked-in `storage/schema/NNN_*.sql` files are meant to document
    /// this list, and a parity test needs to compare against the real thing
    /// rather than a second hand-copied version (audit ARCH-07).
    struct Migration {
        int version;
        std::string sql;
    };

    /// The full ordered list of migrations, from version 1 to the current
    /// schema. This is the single source of truth `runMigrations()` applies;
    /// `storage/schema/NNN_*.sql` are generated-by-hand copies kept in sync
    /// with this list and verified against it by SessionManagerMigrationTest.
    static const std::vector<Migration>& migrations();

    // -------------------------------------------------------------------
    // Transactional outbox (audit ARCH-03): a durable local record that a
    // best-effort mirror to the Python feedback/provenance stores needs to
    // happen, written in the same commit as the local action it describes -
    // so an offline mirror attempt can be retried later instead of silently
    // lost, and a retried delivery can be told apart from a genuinely new one
    // via idempotencyKey (the Python side dedupes on it).
    // -------------------------------------------------------------------

    /// One row of the outbox, as read back for dispatch/inspection.
    struct OutboxEntry {
        long long id = 0;
        std::string idempotencyKey;
        std::string kind;        // e.g. "feedback" or "event"
        std::string payloadJson;
        std::string status;      // "pending" | "delivered" | "failed"
        int attempts = 0;
        std::string createdAt;
        std::string lastError;
    };

    /// Durably records that `kind`/`payloadJson` needs to be mirrored to the
    /// Python side. `idempotencyKey` must be unique per logical event (the
    /// caller generates it, e.g. a UUID) - a duplicate key is treated as
    /// "already enqueued" and succeeds without inserting a second row, so a
    /// caller that retries enqueue() after a crash before commit doesn't
    /// double-enqueue.
    Result<void> enqueueOutboxEvent(const std::string& idempotencyKey, const std::string& kind,
                                     const std::string& payloadJson);

    struct OutboxEnqueue {
        std::string idempotencyKey, kind, payloadJson;
    };
    /// Enqueues several entries atomically (one transaction): either all are
    /// queued or none. Payloads that belong to a session should carry a
    /// "session_id" field so deleteSession() can erase them (AUDIT-014).
    Result<void> enqueueOutboxEvents(const std::vector<OutboxEnqueue>& events);

    /// last_error prefix marking a dead-lettered entry: the service rejected
    /// it permanently (4xx), so it is never retried and no longer counts as
    /// undelivered (AUDIT-056).
    static constexpr const char* kOutboxDeadLetterPrefix = "dead-letter: ";

    /// All entries not yet delivered and not dead-lettered (both "pending"
    /// and "failed" - failed entries are still retry candidates, just with
    /// attempts>0), oldest first, ignoring the retry backoff. Call from
    /// WorkerThread only.
    std::vector<OutboxEntry> pendingOutboxEntries(int limit = 200);

    /// Like pendingOutboxEntries() but only entries whose retry backoff has
    /// elapsed (2 << attempts seconds, capped at 1 h) - what the dispatcher
    /// sends (AUDIT-056).
    std::vector<OutboxEntry> dueOutboxEntries(int limit = 200);

    /// True while `id` still exists and isn't delivered - the dispatcher
    /// re-checks this right before sending, so an entry erased by "Delete all"
    /// mid-pass is not posted.
    bool isOutboxEntryUndelivered(long long id);

    /// Marks an entry as successfully delivered - it will no longer appear
    /// in pendingOutboxEntries().
    Result<void> markOutboxDelivered(long long id);

    /// Marks a delivery attempt as failed, incrementing attempts and
    /// recording the error; the entry remains pending for retry.
    Result<void> markOutboxFailed(long long id, const std::string& error);

    /// Permanently parks an entry the service rejected (attempts++, error
    /// recorded with kOutboxDeadLetterPrefix); it is kept for diagnostics but
    /// never retried.
    Result<void> markOutboxDeadLettered(long long id, const std::string& error);

    /// Count of not-yet-delivered, retryable entries, for a diagnostics view
    /// (audit ARCH-03's "diagnostics view for undelivered events").
    int undeliveredOutboxCount();

    /// Count of dead-lettered entries.
    int deadLetteredOutboxCount();

    /// Deletes delivered entries whose delivery is at least `olderThanDays`
    /// old (0 = all delivered). Returns the number removed.
    int pruneDeliveredOutbox(int olderThanDays = 1);

    // -------------------------------------------------------------------
    // Backup / export (audit DATA-02)
    // -------------------------------------------------------------------

    /// WAL-aware online backup of the entire database to `destPath` using
    /// SQLite's online-backup API (safe while the DB is open and in WAL mode -
    /// no need to stop writes or checkpoint first). The destination is a
    /// self-contained SQLite file a user can archive and later restore by
    /// pointing SessionManager at it. Overwrites `destPath` if it exists.
    Result<void> backupTo(const std::string& destPath);

    /// Portable JSON export of ONE session and its analysis history (audit
    /// DATA-02's "portable export"): the session record plus every
    /// analysis_results row's stored data_json, as a single JSON object the
    /// user can keep independently of the app's SQLite files. Provenance
    /// events (Python-side store) are exported separately by the caller via
    /// the bridge; this covers the khensu.db half.
    Result<std::string> exportSessionJson(const std::string& id);

  private:
    std::unique_ptr<SQLite::Database> m_db;
    /// One connection is shared by the UI and worker threads (autosave,
    /// chat history, plugin scans). SQLite transactions belong to the
    /// connection, so unsynchronised use interleaved one thread's writes into
    /// another's transaction. Every public DB method holds this (recursive:
    /// methods call each other). Heap-held so the class stays movable.
    std::unique_ptr<std::recursive_mutex> m_dbMutex = std::make_unique<std::recursive_mutex>();
    bool m_readOnly = false;

    void runMigrations();
    std::string sessionToJson(const Session& s) const;
    Session sessionFromJson(const std::string& json) const;
    std::string generateId() const;
    std::string utcNow() const;
};

} // namespace khensu
