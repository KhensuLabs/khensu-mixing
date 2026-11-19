// desktop-app/Source/ui/shared/DataLifecycleController.h
#pragma once
#include <JuceHeader.h>
#include <memory>
#include <atomic>
#include <functional>

namespace khensu {

class SessionManager;
class FeedbackManager;

/// Owns data-export, backup, and erasure operations that previously lived
/// inside AppController (CQ-08 split). Keeps AppController focused on app-level
/// orchestration while this unit handles the file/DB dialogs, background I/O,
/// and best-effort Python-side cleanup.
class DataLifecycleController {
  public:
    using BackgroundRunner = std::function<void(std::function<void()>)>;

    DataLifecycleController(SessionManager& sessionMgr, FeedbackManager* feedbackMgr,
                            BackgroundRunner runInBackground, juce::File configDir);

    /// Degraded storage mode (AUDIT-013): khensu.db could not be opened and
    /// the stores run in memory. Scheduled and manual backups are refused -
    /// a backup of the empty in-memory DB would rotate out a good one.
    void setStorageDegraded(bool degraded) noexcept { storageDegraded_ = degraded; }
    bool isStorageDegraded() const noexcept { return storageDegraded_; }

    /// Copies of personal data that "Delete all" does not remove by itself:
    /// scheduled backups (backups/*.db + sidecars + interrupted .tmp files)
    /// and the legacy-store *.bak / *.migrated files (+ -wal/-shm) left by
    /// the DB unification. Offered for deletion after an erasure (AUDIT-014).
    static juce::Array<juce::File> residualDataFiles(const juce::File& configDir);

    /// PKG-4: deletes the DB upgrade's leftovers older than 30 days
    /// (`legacy_store::retirable`, ui/shared/LegacyStoreFiles.h) in the background.
    /// Does nothing in degraded storage mode: khensu.db has to be working
    /// before the copies it was built from go.
    void retireLegacyStoreFiles();

    /// Export one session (khensu.db record + analysis history + Python-side
    /// provenance events) to a user-chosen JSON file. Prompts via a native save
    /// dialog parented to `parent`. Reports outcome via `onDone`.
    void exportSessionToFile(const juce::String& sessionId, const juce::String& suggestedName,
                             juce::Component* parent,
                             std::function<void(bool ok, juce::String messageOrPath)> onDone);

    /// Back up the entire khensu.db to a user-chosen file.
    void backupDatabaseToFile(juce::Component* parent,
                              std::function<void(bool ok, juce::String messageOrPath)> onDone);

    /// Runs a scheduled backup if the setting is on and >= 24 h have passed
    /// since the last success. Safe to call redundantly - an in-flight flag
    /// dedupes. The copy itself runs on the background runner.
    void runScheduledBackupIfDue();

    /// Deletes the session row and everything derived from it in khensu.db
    /// (SessionManager::deleteSession: analysis/suggestion history, prefs,
    /// outbox rows, provenance events), its stems directory, then compacts the
    /// DB in the background and repeats the provenance delete over HTTP
    /// (best-effort, for a service still holding rows in memory). Does NOT
    /// clear the active session pointer or update UI - the caller does that in
    /// `onDone` on success.
    void deleteSessionAndDerivedData(const juce::String& sessionId, const juce::File& stemsDir,
                                     std::function<void(bool ok, juce::String error)> onDone);

    /// Wipes every session (khensu.db, cascading) and every feedback/preference
    /// row plus their Python-side counterparts, compacts the DB, then offers
    /// to delete residualDataFiles(). Does NOT clear the active session
    /// pointer or update UI - the caller does that in `onDone`.
    void deleteAllKhensuData(std::function<void(bool ok, juce::String error)> onDone);

  private:
    SessionManager& sessionMgr_;
    FeedbackManager* feedbackMgr_;
    BackgroundRunner runInBackground_;
    juce::File configDir_;

    // Shared export/backup save dialog. JUCE's launchAsync keeps the chooser
    // alive for the duration of the modal; holding it here is sufficient.
    std::unique_ptr<juce::FileChooser> chooser_;

    void offerResidualFileDeletion();

    // Guards concurrent scheduled backups.
    std::atomic<bool> scheduledBackupInFlight_{false};
    bool storageDegraded_ = false;
    /// Expires with the controller; message-thread callbacks check it first.
    std::shared_ptr<int> lifetime_ = std::make_shared<int>(0);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DataLifecycleController)
};

} // namespace khensu
