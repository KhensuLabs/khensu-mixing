// desktop-app/Source/ui/shared/DataLifecycleController.cpp
#include "DataLifecycleController.h"
#include <khensu/session/SessionManager.h>
#include <khensu/feedback/FeedbackManager.h>
#include <khensu/common/AppPaths.h>
#include <khensu/ipc/HttpServiceBridge.h>
#include "../../settings/OnlineSettings.h"
#include "../../settings/ScheduledBackup.h"
#include "LegacyStoreFiles.h"

namespace khensu {

DataLifecycleController::DataLifecycleController(SessionManager& sessionMgr, FeedbackManager* feedbackMgr,
                                                 BackgroundRunner runInBackground, juce::File configDir)
    : sessionMgr_(sessionMgr), feedbackMgr_(feedbackMgr), runInBackground_(std::move(runInBackground)),
      configDir_(std::move(configDir))
{
}

void DataLifecycleController::exportSessionToFile(const juce::String& sessionId, const juce::String& suggestedName,
                                                  juce::Component* parent,
                                                  std::function<void(bool ok, juce::String messageOrPath)> onDone)
{
    chooser_ = std::make_unique<juce::FileChooser>(
        "Export session data", juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                                   .getChildFile(suggestedName),
        "*.json");
    chooser_->launchAsync(
        juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles |
            juce::FileBrowserComponent::warnAboutOverwriting,
        [this, sessionId, onDone](const juce::FileChooser& fc) {
            const auto chosen = fc.getResult();
            if (chosen == juce::File{}) return; // cancelled

            auto exported = sessionMgr_.exportSessionJson(sessionId.toStdString());
            if (!exported.is_ok()) {
                onDone(false, juce::String(exported.error()));
                return;
            }
            // Fold the Python-side provenance events into the same bundle
            // (best-effort - the khensu.db half above is authoritative).
            juce::var root = juce::JSON::parse(juce::String(exported.value()));
            if (auto* obj = root.getDynamicObject()) {
                ipc::HttpServiceBridge bridge;
                if (auto events = bridge.getEvents(sessionId, 1000); events.is_ok()) {
                    juce::Array<juce::var> evs;
                    for (const auto& e : events.value()) {
                        auto* eo = new juce::DynamicObject();
                        eo->setProperty("event_type", e.event_type);
                        eo->setProperty("summary", e.summary);
                        eo->setProperty("recommendation_id", e.recommendation_id);
                        eo->setProperty("created_at", e.created_at);
                        evs.add(juce::var(eo));
                    }
                    obj->setProperty("provenance_events", evs);
                }
            }
            if (chosen.replaceWithText(juce::JSON::toString(root)))
                onDone(true, chosen.getFullPathName());
            else
                onDone(false, "Could not write the export file.");
        });
}

void DataLifecycleController::backupDatabaseToFile(juce::Component* parent,
                                                   std::function<void(bool ok, juce::String messageOrPath)> onDone)
{
    chooser_ = std::make_unique<juce::FileChooser>(
        "Back up Khensu database", juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                                       .getChildFile("khensu_backup.db"),
        "*.db");
    chooser_->launchAsync(
        juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles |
            juce::FileBrowserComponent::warnAboutOverwriting,
        [this, onDone](const juce::FileChooser& fc) {
            const auto chosen = fc.getResult();
            if (chosen == juce::File{}) return; // cancelled
            if (storageDegraded_) {
                onDone(false, TRANS("Session storage is running in temporary mode, so there is nothing to back up. "
                                    "Restart Khensu once the database is available again."));
                return;
            }
            auto result = sessionMgr_.backupTo(chosen.getFullPathName().toStdString());
            if (result.is_ok())
                onDone(true, chosen.getFullPathName());
            else
                onDone(false, juce::String(result.error()));
        });
}

void DataLifecycleController::runScheduledBackupIfDue()
{
    if (storageDegraded_) return; // AUDIT-013: never back up the in-memory fallback
    const auto settings = OnlineSettings::load(configDir_);
    if (!settings.scheduledBackup) return;
    if (!scheduled_backup::isBackupDue(settings.lastScheduledBackupIso, juce::Time::getCurrentTime())) return;
    if (scheduledBackupInFlight_.exchange(true)) return;

    juce::Component::SafePointer<juce::Component> anchor; // controller lifetime exceeds the job
    runInBackground_([this]() {
        const auto dir = configDir_.getChildFile("backups");
        dir.createDirectory();
        const auto dest = dir.getChildFile(scheduled_backup::backupFileName(juce::Time::getCurrentTime()));
        const auto result = sessionMgr_.backupTo(dest.getFullPathName().toStdString());
        const bool ok = result.is_ok();
        // The controller can be gone by the time this message runs (app quit
        // right after the backup finished): check its lifetime token first.
        juce::MessageManager::callAsync([this, alive = std::weak_ptr<int>(lifetime_), dir, dest, ok]() {
            if (alive.expired()) return;
            if (ok) {
                scheduled_backup::pruneOldBackups(dir);
                // Reload before writing the timestamp so a settings edit made
                // while the copy ran isn't clobbered by our stale snapshot.
                OnlineSettings::update(configDir_, [](OnlineSettings& s) {
                    s.lastScheduledBackupIso = juce::Time::getCurrentTime().toISO8601(true);
                });
                juce::Logger::writeToLog("Scheduled backup written: " + dest.getFullPathName());
            } else {
                // Quiet failure by design: the hourly timer retries, and a
                // popup for a background convenience feature would be noise.
                juce::Logger::writeToLog("Scheduled backup FAILED for " + dest.getFullPathName());
            }
            scheduledBackupInFlight_.store(false);
        });
    });
}

void DataLifecycleController::deleteSessionAndDerivedData(const juce::String& sessionId, const juce::File& stemsDir,
                                                          std::function<void(bool ok, juce::String error)> onDone)
{
    auto delResult = sessionMgr_.deleteSession(sessionId.toStdString());
    if (!delResult.is_ok()) {
        onDone(false, juce::String(delResult.error()));
        return;
    }
    if (stemsDir.exists()) stemsDir.deleteRecursively();

    runInBackground_([this, sessionId, onDone]() {
        // Free pages and the WAL still hold the erased rows (AUDIT-014).
        sessionMgr_.compactAfterErasure();
        ipc::HttpServiceBridge bridge;
        bridge.deleteEventsForSession(sessionId); // best-effort; khensu.db rows are already gone
        juce::MessageManager::callAsync([onDone]() { onDone(true, {}); });
    });
}

void DataLifecycleController::deleteAllKhensuData(std::function<void(bool ok, juce::String error)> onDone)
{
    // Feedback first: its rows share khensu.db, so the compaction below
    // (after the sessions delete) purges both.
    if (feedbackMgr_) {
        if (auto fb = feedbackMgr_->deleteAll(); !fb.is_ok()) {
            onDone(false, juce::String(fb.error()));
            return;
        }
    }
    auto delResult = sessionMgr_.deleteAllSessions();
    if (!delResult.is_ok()) {
        onDone(false, juce::String(delResult.error()));
        return;
    }

    runInBackground_([this, onDone]() {
        sessionMgr_.compactAfterErasure();
        ipc::HttpServiceBridge bridge;
        bridge.deleteAllEvents();   // best-effort
        bridge.deleteAllFeedback(); // best-effort
        juce::MessageManager::callAsync([this, alive = std::weak_ptr<int>(lifetime_), onDone]() {
            onDone(true, {});
            if (!alive.expired()) offerResidualFileDeletion();
        });
    });
}

juce::Array<juce::File> DataLifecycleController::residualDataFiles(const juce::File& configDir)
{
    juce::Array<juce::File> files;
    const auto backups = configDir.getChildFile("backups");
    if (backups.isDirectory())
        for (const auto& pattern : {"*.db", "*.db-wal", "*.db-shm", "*.tmp-*"})
            files.addArray(backups.findChildFiles(juce::File::findFiles, false, pattern));
    if (configDir.isDirectory())
        for (const auto& pattern : {"*.bak", "*.bak-wal", "*.bak-shm", "*.migrated", "*.migrated-wal",
                                    "*.migrated-shm"})
            files.addArray(configDir.findChildFiles(juce::File::findFiles, false, pattern));
    return files;
}

void DataLifecycleController::retireLegacyStoreFiles()
{
    if (storageDegraded_) return;
    const auto files =
        legacy_store::retirable(configDir_, juce::Time::getCurrentTime(), juce::RelativeTime::days(30));
    if (files.isEmpty()) return;
    runInBackground_([files]() {
        int removed = 0;
        for (const auto& f : files)
            if (f.deleteFile()) ++removed;
        juce::Logger::writeToLog("Retired " + juce::String(removed) + " of " + juce::String(files.size()) +
                                 " legacy store file(s) from the khensu.db upgrade (PKG-4)");
    });
}

void DataLifecycleController::offerResidualFileDeletion()
{
    const auto files = residualDataFiles(configDir_);
    if (files.isEmpty()) return;
    juce::AlertWindow::showOkCancelBox(
        juce::MessageBoxIconType::QuestionIcon, TRANS("Delete backups too?"),
        TRANS("Khensu data was deleted, but older copies of it still exist on this computer: automatic database "
              "backups and the files left over from an earlier storage upgrade.") +
            "\n\n" + juce::String(files.size()) + " " + TRANS("file(s) in") + " " + configDir_.getFullPathName() +
            "\n\n" + TRANS("Delete them as well? This cannot be undone."),
        TRANS("Delete Backups"), TRANS("Keep Them"), nullptr,
        juce::ModalCallbackFunction::create([this, alive = std::weak_ptr<int>(lifetime_), files](int result) {
            if (result != 1 || alive.expired()) return;
            runInBackground_([files]() {
                int failed = 0;
                for (const auto& f : files)
                    if (f.existsAsFile() && !f.deleteFile()) ++failed;
                if (failed > 0)
                    juce::Logger::writeToLog("DataLifecycleController: " + juce::String(failed) +
                                             " backup/legacy file(s) could not be deleted");
            });
        }));
}

} // namespace khensu
