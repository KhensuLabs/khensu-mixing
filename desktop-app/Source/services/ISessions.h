// desktop-app/Source/services/ISessions.h
#pragma once
#include <JuceHeader.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <khensu/session/SessionManager.h>

namespace khensu {

struct CreativeTarget;
class SessionScoped;

/// Sessions and their data (ENG-1): open / start / list, the session store,
/// provenance, export, backup and erasure. Message thread unless noted.
class ISessions {
  public:
    using Done = std::function<void(bool ok, juce::String messageOrPath)>;
    virtual ~ISessions() = default;

    virtual bool hasActiveSession() const noexcept = 0;
    virtual const Session* getActiveSession() const noexcept = 0;
    /// The open session's original source file, or "".
    virtual juce::String activeSourcePath() const = 0;
    virtual bool startSession(const juce::String& name, const std::vector<AudioFileRef>& files,
                              const CreativeTarget& target) = 0;
    /// WF-7: one session per file (named after it, the same target), grouped
    /// as one project in drop order (FEAT-6). The first opens like
    /// startSession(); the others are saved and listed with the recent
    /// sessions. Returns how many were created (0 = none saved).
    virtual int startSessionPerFile(const std::vector<AudioFileRef>& files, const CreativeTarget& target) = 0;
    virtual bool loadSessionById(const std::string& id) = 0;

    // ---- Projects (FEAT-6) ----
    /// Tells the project listeners a project changed (made, renamed, a track
    /// added, removed or moved). Whoever wrote the change calls it.
    virtual void notifyProjectsChanged() = 0;
    virtual void addProjectsListener(void* key, std::function<void()> cb) = 0;
    virtual void removeProjectsListener(void* key) = 0;
    virtual std::vector<Session> listRecentSessions(int limit = 10) = 0;
    virtual std::vector<SessionManager::AnalysisSummary> listRecentAnalysisSummaries(int limit = 10) = 0;

    /// The store; sessionManagerPtr() keeps it alive for background jobs.
    virtual SessionManager& sessionManager() noexcept = 0;
    virtual std::shared_ptr<SessionManager> sessionManagerPtr() const noexcept = 0;
    /// Replaces the open session's target and saves it (off the message
    /// thread). False without an open session.
    virtual bool updateActiveSessionTarget(const CreativeTarget& target) = 0;
    /// WF-12: renames a session, or sets its version note; the open
    /// session's in-memory copy (and the sidebar) follow, so a later save of
    /// it does not undo the change. False when it could not be saved.
    virtual bool renameSession(const std::string& id, const juce::String& name) = 0;
    virtual bool setSessionNote(const std::string& id, const juce::String& note) = 0;

    /// Durable provenance event (transactional outbox).
    virtual void enqueueProvenanceEvent(const juce::String& sessionId, const juce::String& eventType,
                                        const juce::String& summary, const juce::String& recommendationId = {},
                                        const juce::var& payload = {}) = 0;

    // ---- Session-reset fan-out ----
    virtual void addSessionScoped(SessionScoped* scoped) = 0;
    virtual void removeSessionScoped(SessionScoped* scoped) = 0;

    // ---- Data (DATA-02); callers confirm destructive actions first ----
    /// Portable JSON export of the active session (its khensu.db record +
    /// analysis history, plus its Python-side provenance events) to a file
    /// the user picks. Error via `onDone` without an active session.
    virtual void exportActiveSessionToFile(Done onDone) = 0;
    /// Online, WAL-aware backup of the whole khensu.db to a file the user picks.
    virtual void backupDatabaseToFile(Done onDone) = 0;
    /// The automatic daily backup: runs if the setting is on and >= 24 h have
    /// passed since the last success; the copy runs in the background and an
    /// in-flight flag dedupes redundant calls.
    virtual void runScheduledBackupIfDue() = 0;
    /// Irreversible. Deletes the ACTIVE session: its row (cascading
    /// analysis_results / suggestion_history), its stems directory (only
    /// resolvable for the active session - stems live next to its source
    /// file) and, best-effort, its Python-side provenance events. Feedback
    /// rows are keyed by recommendation_id with no link back to a session, so
    /// they stay (docs/data-inventory.md). Clears the active session on success.
    virtual void deleteActiveSessionAndDerivedData(std::function<void(bool ok, juce::String error)> onDone) = 0;
    /// Irreversible. Wipes every session and every feedback / preference /
    /// override row, plus (best-effort) their Python-side counterparts. Stem
    /// files next to source audio stay (no app-owned root to bulk-delete
    /// safely), as do scanned_plugins and workspace settings.
    virtual void deleteAllKhensuData(std::function<void(bool ok, juce::String error)> onDone) = 0;
};

} // namespace khensu
