// desktop-app/Source/AppController_Session.cpp
// Session lifecycle (audit refactor 'Split AppController' - SessionLifecycle): start / load / delete / relink a session, session-scoped state reset, autosave + restore, data export/backup. The vocal undo stack and the stems folder moved to session/VocalEditService and session/StemService (ENG-1 stage C3).
// Split out of AppController.cpp; see AppController.h for the declarations.
#include "AppController.h"
#include "session/ProjectTracks.h"
#include "ui/shared/AbxTrialOverlay.h"
#include <khensu/targets/UserPreferenceModel.h>
#include "session/WorkspaceRecoveryState.h"
#include "ui/shared/RenderPaths.h"
#include "ui/shared/DecisionNotices.h"
#include <algorithm>

namespace khensu {


bool AppController::startSession(const juce::String& name, const std::vector<AudioFileRef>& files,
                                 const CreativeTarget& target)
{
    auto session = sessions_.create(name, files, target);
    if (!session) return false;

    invalidateBackgroundReads();
    resetSessionScopedState();
    sessions_.adopt(std::move(*session));
    stems_.clearVocalAnalysis();
    restoreAutosavedState();
    loadTargetReference();
    updateSessionStatusLabel();
    showScreen(ScreenType::Dashboard);
    if (!sessions_.state().session->files.empty())
        playback_.loadAudioFileForPlayback(juce::File(activeSourcePath()));
    // Stem separation (Demucs) saturates the CPU; started alongside the
    // analysis it starved the deep pass past its deadline. It now starts when
    // the results are released (the gate opens, applyResultsGate).
    stemsAutoStartPending_ = true;
    analysis_.beginAnalysis();
    maybeShowCoach(); // UX-15
    return true;
}

int AppController::startSessionPerFile(const std::vector<AudioFileRef>& files, const CreativeTarget& target)
{
    if (files.empty()) return 0;
    auto nameOf = [](const AudioFileRef& f) {
        return f.title.empty() ? juce::File(juce::String(f.path)).getFileNameWithoutExtension() : juce::String(f.title);
    };
    // The others first, so the opened one is the newest of the batch.
    int created = 0;
    std::vector<std::string> ids(files.size());
    for (size_t i = 1; i < files.size(); ++i)
        if (auto s = sessions_.create(nameOf(files[i]), {files[i]}, target)) {
            ids[i] = s->id;
            ++created;
        }
    if (startSession(nameOf(files[0]), {files[0]}, target)) {
        ids[0] = sessions_.state().session->id;
        ++created;
    }
    // FEAT-6: the drop's sessions are one project, in drop order.
    if (created > 1) {
        auto& db = sessions_.manager();
        if (auto project = db.createProject(project_tracks::nameForFiles(files).toStdString()); project.is_ok()) {
            for (const auto& id : ids)
                if (!id.empty()) db.addSessionToProject(project.value().id, id);
            notifyProjectsChanged();
        }
    }
    return created;
}

void AppController::notifyProjectsChanged()
{
    const auto listeners = projectsListeners_; // a listener may deregister itself
    for (const auto& [key, cb] : listeners)
        if (projectsListeners_.count(key) != 0 && cb) cb();
}

bool AppController::loadSessionById(const std::string& id)
{
    auto loaded = sessions_.load(id);
    if (loaded.status == SessionService::LoadStatus::Error) {
        // "Couldn't read" and "doesn't exist" are different states (audit
        // CQ-05, now a typed result) - a locked/corrupt database must not
        // read as a silently-dead recent-sessions list.
        showServiceError(TRANS("This session could not be loaded: the session database may be locked or "
                               "temporarily unavailable.") +
                         " " + juce::String(loaded.error)); // UX-10
        return false;
    }
    if (loaded.status == SessionService::LoadStatus::NotFound) return false; // genuinely no such session

    invalidateBackgroundReads();
    resetSessionScopedState();
    sessions_.adopt(std::move(*loaded.session));
    stems_.clearVocalAnalysis();
    restoreAutosavedState();
    loadTargetReference();
    updateSessionStatusLabel();
    showScreen(ScreenType::Dashboard);

    if (!sessions_.state().session->files.empty()) {
        juce::File f(sessions_.state().session->files[0].path);
        if (f.existsAsFile()) {
            playback_.loadAudioFileForPlayback(f);
            analysis_.beginAnalysis();
        } else {
            // DSK-02: nothing of the previous session may stay live here -
            // resetSessionScopedState() already cleared the analysis, the
            // transport and the waveform; the results gate says why there
            // is nothing to show (with a re-run once the file is located).
            analysis_.setResultsGate(AnalysisService::ResultsGate::Failed);
            resultsOverlay_.setMode(AnalysisPendingOverlay::Mode::Failed,
                                    TRANS("This session's audio file could not be found.") + "\n" +
                                        juce::String(sessions_.state().session->files[0].path));
            handleMissingSessionFile();
        }
    } else {
        analysis_.setResultsGate(AnalysisService::ResultsGate::Open); // nothing to analyse, nothing withheld
    }
    return true;
}


void AppController::handleMissingSessionFile()
{
    if (!sessions_.state().session || sessions_.state().session->files.empty()) return;

    const juce::String missingPath(sessions_.state().session->files[0].path);
    sidebar_.setAnalysisStatus(TRANS("File not found"), false);

    const juce::String sessionId(sessions_.state().session->id);
    // A notice, not a modal box: locating the file is one non-destructive
    // action, and dismissing it is the old "Skip" (the session stays open
    // without playback/analysis). The notice can outlive the session, so the
    // action re-checks it.
    juce::Component::SafePointer<AppController> self(this);
    auto stillSameSession = [self, sessionId] {
        return self != nullptr && self->sessions_.state().session &&
               juce::String(self->sessions_.state().session->id) == sessionId;
    };
    auto locate = [self, sessionId] {
        if (self == nullptr) return;
        self->missingFileChooser_ = std::make_unique<juce::FileChooser>(
            TRANS("Locate the missing audio file"), juce::File::getSpecialLocation(juce::File::userMusicDirectory),
            "*.wav,*.aiff,*.flac,*.mp3,*.aac,*.ogg");
        self->missingFileChooser_->launchAsync(
            juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
            [self, sessionId](const juce::FileChooser& fc) {
                if (self == nullptr) return;
                auto chosen = fc.getResult();
                auto& sessions = self->sessions_;
                if (!chosen.existsAsFile() || !sessions.state().session) return;
                if (juce::String(sessions.state().session->id) != sessionId) return; // session changed meanwhile

                self->invalidateBackgroundReads();
                // Off the message thread (DSK-20): WAL + busy_timeout can stall.
                if (auto relinked = sessions.relinkSource(chosen.getFullPathName()))
                    self->runInBackground([sm = sessions.sharedManager(), session = std::move(*relinked)]() mutable {
                        sm->saveSession(session);
                    });

                self->playback_.loadAudioFileForPlayback(chosen);
                self->updateSessionStatusLabel();
                self->analysis_.beginAnalysis();
            });
    };
    decision_notices::postReplacing(
        noticeBar_, decision_notices::missingSessionFile(missingPath, std::move(stillSameSession), std::move(locate)));
}


void AppController::loadTargetReference()
{
    // A reference named at intake (P0-1/P0-4) is the session's comparison
    // reference, unless the workspace autosave already restored one.
    if (!sessions_.state().session || analysis_.getReferenceTrack().isNotEmpty()) return;
    for (const auto& path : sessions_.state().session->target.reference_track_paths) {
        const juce::File reference{juce::String(path)};
        if (juce::File::isAbsolutePath(juce::String(path)) && reference.existsAsFile()) {
            analysis_.setReferenceTrack(reference.getFullPathName());
            return;
        }
    }
}

bool AppController::updateActiveSessionTarget(const CreativeTarget& target)
{
    auto updated = sessions_.replaceTarget(target);
    if (!updated) return false;
    // Off the message thread (DSK-20): WAL + busy_timeout can stall.
    runInBackground([sm = sessions_.sharedManager(), session = std::move(*updated)]() mutable {
        if (auto saved = sm->saveSession(session); saved.is_err())
            juce::Logger::writeToLog("AppController: session target not saved: " + juce::String(saved.error()));
    });
    return true;
}

bool AppController::renameSession(const std::string& id, const juce::String& name)
{
    const auto trimmed = name.trim();
    if (trimmed.isEmpty() || sessionManager().renameSession(id, trimmed.toStdString()).is_err()) return false;
    sessions_.applyRename(id, trimmed.toStdString());
    updateSessionStatusLabel();
    return true;
}

bool AppController::setSessionNote(const std::string& id, const juce::String& note)
{
    if (sessionManager().setSessionNote(id, note.trim().toStdString()).is_err()) return false;
    sessions_.applyNote(id, note.trim().toStdString());
    return true;
}

std::vector<Session> AppController::listRecentSessions(int limit)
{
    return sessions_.listRecent(limit);
}


std::vector<SessionManager::AnalysisSummary> AppController::listRecentAnalysisSummaries(int limit)
{
    return sessions_.listRecentAnalysisSummaries(limit);
}


void AppController::deleteActiveSessionAndDerivedData(std::function<void(bool ok, juce::String error)> onDone)
{
    if (!sessions_.state().session) {
        onDone(false, "No active session.");
        return;
    }
    const juce::String sessionId(sessions_.state().session->id);
    const auto stemsDir = stems_.getStemsDirectory();
    // Renders this session produced are derived data too and were left on
    // disk. AUDIT-058 policy: delete a file only if it is in this session's
    // render history (every branch of the tree, not just the linear stack)
    // AND carries uniqueRenderFile()'s naming pattern for one of the session's
    // sources AND is not itself a source - the user's own audio is never
    // touched.
    std::vector<juce::File> renders;
    for (const auto& node : renders_.renderTree()) {
        const juce::File f(node.path);
        bool isSource = false;
        bool appRender = false;
        for (const auto& ref : sessions_.state().session->files) {
            const juce::File source(juce::String(ref.path));
            isSource = isSource || f == source;
            appRender = appRender || isAppRenderFileFor(f, source);
        }
        if (!isSource && appRender && std::find(renders.begin(), renders.end(), f) == renders.end())
            renders.push_back(f);
    }

    jassert(dataLifecycleController_.has_value());
    dataLifecycleController_->deleteSessionAndDerivedData(
        sessionId, stemsDir, [this, onDone, renders](bool ok, juce::String error) {
            if (ok) {
                for (const auto& f : renders)
                    if (f.existsAsFile()) f.deleteFile();
                invalidateBackgroundReads();
                resetSessionScopedState();
                sessions_.state().session.reset();
                updateSessionStatusLabel();
            }
            onDone(ok, error);
        });
}


void AppController::deleteAllKhensuData(std::function<void(bool ok, juce::String error)> onDone)
{
    jassert(dataLifecycleController_.has_value());
    dataLifecycleController_->deleteAllKhensuData([this, onDone](bool ok, juce::String error) {
        if (ok) {
            invalidateBackgroundReads();
            resetSessionScopedState();
            sessions_.state().session.reset();
            updateSessionStatusLabel();
        }
        onDone(ok, error);
    });
}


void AppController::resetSessionScopedState()
{
    stopLoopRegion(); // FEAT-8: a looped finding belongs to the session being left
    // DSK-21: the ABX overlay borrows the A/B B slot and the loaded side;
    // close it FIRST, while its saved state still belongs to the session
    // being left (it restores into that session), then reset everything.
    hideAbxTrial();

    stemsAutoStartPending_ = false; // only startSession() re-arms it
    // Every queued/running render belongs to the session being left: cancel
    // them so none can finish into the next session's history, A/B side,
    // autosave or export (AUDIT-002). Callbacks fire now with ok=false.
    renders_.resetForSession(); // + render history and the delta-solo source
    noticeBar_.withdraw("rerun-offer"); // AUTO-3: an offer for the session being left
    noticeBar_.withdraw(decision_notices::kMissingSessionFileId); // the left session's missing file
    // After the renders: a cancelled fix has reported by now. The issue
    // queue then forgets the session's states and fix renders (REF-2).
    issues_.resetForSession();

    vocalEdit_.resetForSession(); // vocal undo, strategy and macros back to defaults

    // Stem solo, separation result and vocal analysis; an in-flight
    // separation of the old session stops (AUDIT-067).
    stems_.resetForSession();

    // The reference track (cleared without an autosave, AUDIT-039), the
    // in-flight analysis and its result, and the gate state (DSK-02).
    analysis_.resetForSession();
    analysisState_ = SessionStateStrip::State::Analysis::None;
    refreshSessionStateStrip();
    intakeProgressHud_.reset();
    // The A/B, preview and loaded file of the old session; an in-flight peak
    // scan of the old file is dropped.
    playback_.resetForSession();

    // S4: every registered session-scoped component (Dashboard panels,
    // reference compare, chat, ...) forgets the old session here.
    sessions_.resetAllScoped();
    updateResultsOverlay();

    playback_.updatePlaybackGain();
}


void AppController::exportActiveSessionToFile(std::function<void(bool ok, juce::String messageOrPath)> onDone)
{
    if (!sessions_.state().session) {
        onDone(false, "No active session to export.");
        return;
    }
    const juce::String sessionId(sessions_.state().session->id);
    const juce::String suggestedName =
        juce::File::createLegalFileName(juce::String(sessions_.state().session->name)) + "_khensu_export.json";

    jassert(dataLifecycleController_.has_value());
    dataLifecycleController_->exportSessionToFile(sessionId, suggestedName, this, std::move(onDone));
}


void AppController::runScheduledBackupIfDue()
{
    jassert(dataLifecycleController_.has_value());
    dataLifecycleController_->runScheduledBackupIfDue();
}


void AppController::backupDatabaseToFile(std::function<void(bool ok, juce::String messageOrPath)> onDone)
{
    jassert(dataLifecycleController_.has_value());
    dataLifecycleController_->backupDatabaseToFile(this, std::move(onDone));
}


void AppController::autosaveState()
{
    if (!sessions_.state().session) return;

    const std::string sessionId = sessions_.state().session->id;
    const std::string strategy = vocalEdit_.getSelectedVocalStrategy().toStdString();

    juce::DynamicObject::Ptr obj = new juce::DynamicObject();
    for (const auto& [name, val] : vocalEdit_.macroValues()) {
        obj->setProperty(name, val);
    }
    const std::string macrosJson = juce::JSON::toString(juce::var(obj.get())).toStdString();
    const WorkspaceRecoveryState workspace{renders_.currentRenderPath(), analysis_.getReferenceTrack(),
                                           vocalEdit_.sessionTakes(), stems_.currentVocalStem()};
    const std::string workspaceJson = workspaceRecoveryStateToJson(workspace).toStdString();

    std::shared_ptr<SessionManager> localSessionMgr = sessions_.sharedManager();
    if (!localSessionMgr || !localSessionMgr->isOpen()) return;

    // Latest-wins, single writer: a macro drag calls this per step, and the
    // old thread-per-call could land writes out of order (an older snapshot
    // overwriting a newer one). One worker drains the newest pending payload.
    auto queue = autosaveQueue_;
    bool startWorker = false;
    {
        const std::lock_guard<std::mutex> lock(queue->mutex);
        queue->pending = AutosaveQueue::Payload{sessionId, strategy, macrosJson, workspaceJson};
        startWorker = !queue->workerRunning;
        queue->workerRunning = true;
    }
    if (!startWorker) return;
    runInBackground([queue, localSessionMgr]() {
        for (;;) {
            AutosaveQueue::Payload p;
            {
                const std::lock_guard<std::mutex> lock(queue->mutex);
                if (!queue->pending.has_value()) {
                    queue->workerRunning = false;
                    return;
                }
                p = std::move(*queue->pending);
                queue->pending.reset();
            }
            localSessionMgr->setUserPreference("vocal_strategy_" + p.sessionId, p.strategy);
            localSessionMgr->setUserPreference("vocal_macros_" + p.sessionId, p.macrosJson);
            localSessionMgr->setUserPreference("workspace_recovery_" + p.sessionId, p.workspaceJson);
        }
    });
}


void AppController::restoreAutosavedState()
{
    // DB reads run on a worker (they previously blocked the message thread on
    // every session open); results are applied back on the message thread.
    if (!sessions_.state().session) return;

    const std::string sessionId = sessions_.state().session->id;
    std::shared_ptr<SessionManager> localSessionMgr = sessions_.sharedManager();
    if (!localSessionMgr || !localSessionMgr->isOpen()) return;

    juce::Component::SafePointer<AppController> self(this);
    runInBackground([self, localSessionMgr, sessionId]() {
        std::string strategy, macrosJson, workspaceJson;
        auto stratRes = localSessionMgr->getUserPreference("vocal_strategy_" + sessionId);
        if (stratRes.is_ok()) strategy = stratRes.value();
        auto macrosRes = localSessionMgr->getUserPreference("vocal_macros_" + sessionId);
        if (macrosRes.is_ok()) macrosJson = macrosRes.value();
        auto workspaceRes = localSessionMgr->getUserPreference("workspace_recovery_" + sessionId);
        if (workspaceRes.is_ok()) workspaceJson = workspaceRes.value();
        if (strategy.empty() && macrosJson.empty() && workspaceJson.empty()) return;

        juce::MessageManager::callAsync([self, sessionId, strategy, macrosJson, workspaceJson]() {
            if (self == nullptr) return;
            // The user may have switched sessions while this DB read was in
            // flight; applying a previous session's autosaved vocal strategy/
            // macros onto the now-current one would silently corrupt its
            // state (audit CQ-06).
            if (!self->sessions_.state().session || self->sessions_.state().session->id != sessionId) return;
            // Restores set the values without pushing undo or re-autosaving.
            if (!strategy.empty()) self->vocalEdit_.restoreStrategy(juce::String(strategy));
            if (!macrosJson.empty()) {
                auto json = juce::JSON::parse(juce::String(macrosJson));
                if (auto* objPtr = json.getDynamicObject()) self->vocalEdit_.restoreMacros(*objPtr);
            }
            if (const auto workspace = workspaceRecoveryStateFromJson(juce::String(workspaceJson))) {
                if (workspace->referencePath.isNotEmpty() && juce::File(workspace->referencePath).existsAsFile())
                    self->analysis_.setReferenceTrack(workspace->referencePath);
                if (workspace->renderPath.isNotEmpty() && juce::File(workspace->renderPath).existsAsFile())
                    self->registerAppliedRender(workspace->renderPath, juce::String(sessionId));
                if (!workspace->takes.isEmpty()) self->vocalEdit_.restoreTakes(workspace->takes);
                if (workspace->vocalPath.isNotEmpty()) self->stems_.restoreCurrentVocalStem(workspace->vocalPath);
            }
        });
    });
}

} // namespace khensu
