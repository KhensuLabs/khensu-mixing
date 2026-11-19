// desktop-app/Source/session/SessionService.h
#pragma once
#include <JuceHeader.h>
#include <khensu/session/Session.h>
#include <khensu/session/SessionManager.h>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "SessionState.h"
#include "../ui/shared/SessionScoped.h"

namespace khensu {

/// The session store and "the session currently open" (ENG-1 step 3b,
/// extracted from AppController). Owns:
///  - the SessionManager (khensu.db), opened with retry / degraded fallback;
///  - SessionState (the open session plus its per-session derived data);
///  - the recent-sessions / recent-analyses load-failure flags;
///  - the SessionScoped reset registry.
///
/// It does the database half of starting / loading / relinking a session and
/// nothing on screen. AppController still orchestrates a session switch
/// (invalidate background reads, reset session-scoped state, adopt, load
/// audio, analyse, show the Dashboard); that moves to the shell later.
/// Message-thread only, except sharedManager(), whose SessionManager is
/// thread-safe and is handed to background jobs.
class SessionService {
  public:
    struct StoreOpenResult {
        std::shared_ptr<SessionManager> manager; ///< never null
        bool degraded = false;                   ///< running on ":memory:"
        juce::String failure;                    ///< why the real store failed
    };

    /// Opens `dbPath`, retrying a busy/locked database with backoff
    /// (AUDIT-013). A schema newer than this build is not retried. When the
    /// store can't be opened, falls back to an in-memory one and reports
    /// `degraded` so the caller can warn the user and pause backups.
    static StoreOpenResult openStore(const std::string& dbPath, int attempts = 3);

    /// Attaches the opened store. Called once, early in AppController's
    /// constructor; the store is needed before any session can open.
    void attachStore(std::shared_ptr<SessionManager> manager) { manager_ = std::move(manager); }

    SessionManager& manager() noexcept
    {
        jassert(manager_ != nullptr);
        return *manager_;
    }
    /// Owning handle for background jobs (keeps the store alive past teardown).
    std::shared_ptr<SessionManager> sharedManager() const noexcept { return manager_; }

    // ---- The open session ---------------------------------------------------
    SessionState& state() noexcept { return state_; }
    const SessionState& state() const noexcept { return state_; }
    bool hasSession() const noexcept { return state_.hasSession(); }
    const Session* active() const noexcept { return state_.session.get(); }
    /// The open session's original source file, or "" without one.
    juce::String activeSourcePath() const
    {
        return (state_.session && !state_.session->files.empty()) ? juce::String(state_.session->files[0].path)
                                                                  : juce::String();
    }
    juce::String activeSessionId() const { return state_.session ? juce::String(state_.session->id) : juce::String(); }

    /// Makes `session` the open one. Callers reset the session-scoped state
    /// FIRST (see AppController::resetSessionScopedState).
    void adopt(Session session) { state_.session = std::make_unique<Session>(std::move(session)); }

    // ---- Database half of starting / loading a session ----------------------
    /// Creates and saves a session holding `files`. Nothing is left behind on
    /// failure (a created-but-unsaved record is deleted).
    std::optional<Session> create(const juce::String& name, const std::vector<AudioFileRef>& files,
                                  const CreativeTarget& target);

    enum class LoadStatus { Loaded, NotFound, Error };
    struct LoadOutcome {
        LoadStatus status = LoadStatus::NotFound;
        std::optional<Session> session; ///< set when Loaded
        std::string error;              ///< set when Error ("couldn't read" != "doesn't exist", CQ-05)
    };
    LoadOutcome load(const std::string& id);

    /// Points the open session's source at `newPath` and returns a copy to
    /// persist off the message thread (DSK-20: WAL + busy_timeout can stall).
    /// Empty without an open session.
    std::optional<Session> relinkSource(const juce::String& newPath);
    /// Replaces the open session's target and returns a copy to persist off
    /// the message thread. Empty without an open session.
    std::optional<Session> replaceTarget(const CreativeTarget& target);
    /// WF-12: the open session's copy follows a rename / note (no-op for
    /// another session).
    void applyRename(const std::string& id, const std::string& name)
    {
        if (state_.session && state_.session->id == id) state_.session->name = name;
    }
    void applyNote(const std::string& id, const std::string& note)
    {
        if (state_.session && state_.session->id == id) state_.session->note = note;
    }

    // ---- Recent lists (failure is a state, not an empty list) --------------
    std::vector<Session> listRecent(int limit);
    bool recentLoadFailed() const noexcept { return recentLoadFailed_; }
    std::vector<SessionManager::AnalysisSummary> listRecentAnalysisSummaries(int limit);
    bool recentAnalysisSummariesLoadFailed() const noexcept { return recentAnalysisSummariesLoadFailed_; }

    // ---- Session-reset fan-out (ui/shared/SessionScoped.h) ------------------
    void addScoped(SessionScoped* scoped) { registry_.add(scoped); }
    void removeScoped(SessionScoped* scoped) { registry_.remove(scoped); }
    void resetAllScoped() { registry_.resetAll(); }

  private:
    std::shared_ptr<SessionManager> manager_;
    SessionState state_;
    SessionResetRegistry registry_;
    bool recentLoadFailed_ = false;
    bool recentAnalysisSummariesLoadFailed_ = false;
};

} // namespace khensu
