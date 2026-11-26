// desktop-app/Source/ui/SessionIntake/IntakeScreen.cpp
#include "IntakeScreen.h"
#include "../shared/ButtonRole.h"
#include <khensu/ipc/generated/ApiJobStatus.h>
#include "../shared/BackgroundStop.h"
#include "IntakeQueueSummary.h"
#include "GenreEvidenceText.h"
#include "../../IAppController.h"
#include "../shared/KhensuTheme.h"
#include "../shared/KhensuLookAndFeel.h"
#include <khensu/session/AudioFileProber.h>
#include <khensu/ipc/HttpServiceBridge.h>
#include <khensu/targets/GenreEntry.h>
#include <khensu/targets/GenreDatabase.h>
#include <khensu/targets/GenreConfirmation.h>
#include <khensu/ipc/generated/ApiSchema.h>
#include "../shared/JobPoller.h"
#include "../../PythonServiceManager.h"
#include "ServiceWaitPolicy.h"
#include "TrendRows.h"
#include <atomic>

namespace af = khensu::api::fields;

namespace khensu {

// Every cached palette colour on this component, in one place (UI-01).
void IntakeScreen::applyThemeColours()
{
    genreContextLabel_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
    genreTargetsToggle_.setColour(juce::ToggleButton::textColourId, KhensuTheme::textSecondary);
    confirmCandidateToggle_.setColour(juce::ToggleButton::textColourId, KhensuTheme::textSecondary);
    serviceStatusLabel_.setColour(juce::Label::textColourId, KhensuTheme::accentAmber);
}

IntakeScreen::IntakeScreen(IAppController& ctrl) : controller(ctrl)
{
    applyThemeColours();
    titleLabel.setScreen(ScreenType::Intake);
    openFileButton_.setTooltip(TRANS("Choose an audio file to analyse (Ctrl+O)"));
    openFileButton_.onClick = [this] { dropZone.browse(); };
    titleLabel.setAction(&openFileButton_);
    addAndMakeVisible(titleLabel);

    dropZone.onFilesChosen = [this](const std::vector<juce::File>& fs) { onFilesChosen(fs); };
    addAndMakeVisible(dropZone);
    addAndMakeVisible(queueList_);
    nativeTiles_ = std::make_unique<NativeMeterTiles>(
        [this](std::function<void()> job) { controller.runInBackground(std::move(job)); });
    addAndMakeVisible(*nativeTiles_);

    genreContextLabel_.setText(TRANS("Genre context: waiting for intake"), juce::dontSendNotification);
    genreContextLabel_.setFont(KhensuTheme::bodyFont(type::caption));
    genreContextLabel_.setJustificationType(juce::Justification::topLeft);
    addAndMakeVisible(genreContextLabel_);
    // Item ids, not texts, carry the meaning (currentIntakeSettings), so the
    // labels can be translated.
    genreSourceCombo_.addItem(TRANS("Use detected candidates (confirmation required)"), 1);
    genreSourceCombo_.addItem(TRANS("Use my stated genre"), 2);
    genreSourceCombo_.addItem(TRANS("Match reference track"), 3);
    genreSourceCombo_.addItem(TRANS("Disable genre targets"), 4);
    genreSourceCombo_.setSelectedId(1, juce::dontSendNotification);
    genreSourceCombo_.onChange = [this] {
        if (!probedFiles_.empty()) refreshFullIntakeContext(probedFiles_[0].path);
        updateDetectedLabel();
        resized();
    };
    addAndMakeVisible(genreSourceCombo_);
    genreTargetsToggle_.setToggleState(true, juce::dontSendNotification);
    genreTargetsToggle_.onClick = [this] {
        if (!probedFiles_.empty()) refreshFullIntakeContext(probedFiles_[0].path);
        updateDetectedLabel();
        resized();
    };
    addAndMakeVisible(genreTargetsToggle_);
    confirmCandidateToggle_.onClick = [this] {
        if (!probedFiles_.empty()) refreshFullIntakeContext(probedFiles_[0].path);
    };
    addAndMakeVisible(confirmCandidateToggle_);
    referencePathEditor_.setTextToShowWhenEmpty(TRANS("Optional reference path"), KhensuTheme::textTertiary);
    // A full intake per keystroke used to start while a path was typed:
    // commit on Return or when the field loses focus instead.
    referencePathEditor_.onReturnKey = [this] { commitReferencePath(); };
    referencePathEditor_.onFocusLost = [this] { commitReferencePath(); };
    addAndMakeVisible(referencePathEditor_);
    chooseReferenceButton_.setTooltip(TRANS("Pick the reference track to match and compare against."));
    chooseReferenceButton_.onClick = [this] { chooseReferenceFile(); };
    addAndMakeVisible(chooseReferenceButton_);
    // The stage keys are the bundle's results/stage_status keys; the combo
    // shows a label for each (UX-6: it used to show the snake_case keys).
    const std::pair<const char*, const char*> stages[] = {
        {af::FullIntakeResults::quick_stats, NEEDS_TRANS("Quick stats")},
        {af::FullIntakeResults::signal_integrity, NEEDS_TRANS("Signal integrity")},
        {af::FullIntakeResults::source_triage, NEEDS_TRANS("Source triage")},
        {af::FullIntakeResults::pan_balance, NEEDS_TRANS("Pan balance")},
        {af::FullIntakeResults::artifacts, NEEDS_TRANS("Artifacts")},
        {af::FullIntakeResults::musical_context, NEEDS_TRANS("Musical context")},
        {af::FullIntakeResults::stem_context, NEEDS_TRANS("Stem context")}};
    for (const auto& [key, label] : stages) {
        intakeStageKeys_.add(key);
        intakeStageCombo_.addItem(juce::translate(label), intakeStageCombo_.getNumItems() + 1);
    }
    intakeStageCombo_.setSelectedId(6, juce::dontSendNotification);
    intakeStageCombo_.onChange = [this] { showSelectedStageStatus(); };
    addAndMakeVisible(intakeStageCombo_);
    retryStageButton_.onClick = [this] { retrySelectedIntakeStage(); };
    addAndMakeVisible(retryStageButton_);
    openStagePanelButton_.onClick = [this] { jumpToSelectedIntakeStage(); };
    openStagePanelButton_.setTooltip(TRANS("Open the workspace where this intake stage is explained."));
    addAndMakeVisible(openStagePanelButton_);
    cancelIntakeButton_.setEnabled(false);
    cancelIntakeButton_.onClick = [this] { cancelFullIntakeJob(); };
    addAndMakeVisible(cancelIntakeButton_);

    queueList_.onReorder = [this](int oldIndex, int newIndex) { onQueueReorder(oldIndex, newIndex); };
    queueList_.onCancel = [this](int index) { onQueueCancel(index); };

    targetPanel = std::make_unique<TargetSetupPanel>(controller.genreDatabase());
    addAndMakeVisible(*targetPanel); // static, fills its column (no scroll)

    recentPanel.onSessionChosen = [this](const std::string& id) { controller.sessions().loadSessionById(id); };
    // WF-12: rename, version note and duplicate from a card's right-click menu.
    auto askText = [this](const juce::String& title, const juce::String& current, const juce::String& okText,
                          std::function<void(const juce::String&)> onOk) {
        auto* w = new juce::AlertWindow(title, {}, juce::MessageBoxIconType::NoIcon, this);
        w->addTextEditor("text", current);
        w->addButton(okText, 1, juce::KeyPress(juce::KeyPress::returnKey));
        w->addButton(TRANS("Cancel"), 0, juce::KeyPress(juce::KeyPress::escapeKey));
        w->enterModalState(true, juce::ModalCallbackFunction::create([w, onOk = std::move(onOk)](int result) {
                               if (result == 1) onOk(w->getTextEditorContents("text"));
                           }),
                           true);
    };
    recentPanel.onRenameSession = [this, askText](const Session& session) {
        juce::Component::SafePointer<IntakeScreen> self(this);
        askText(TRANS("Rename session"), juce::String::fromUTF8(session.name.c_str()), TRANS("Rename"),
                [self, id = session.id](const juce::String& name) {
                    if (self == nullptr) return;
                    if (!self->controller.sessions().renameSession(id, name))
                        self->controller.shell().showServiceError(TRANS("The session could not be renamed."));
                    self->refreshRecentSessions();
                });
    };
    recentPanel.onEditSessionNote = [this, askText](const Session& session) {
        juce::Component::SafePointer<IntakeScreen> self(this);
        askText(TRANS("Version note"), juce::String::fromUTF8(session.note.c_str()), TRANS("Save"),
                [self, id = session.id](const juce::String& note) {
                    if (self == nullptr) return;
                    if (!self->controller.sessions().setSessionNote(id, note))
                        self->controller.shell().showServiceError(TRANS("The note could not be saved."));
                    self->refreshRecentSessions();
                });
    };
    recentPanel.onDuplicateSession = [this](const Session& session) {
        auto copy = controller.sessions().sessionManager().duplicateSession(session.id, {});
        if (copy.is_err())
            controller.shell().showServiceError(TRANS("The session could not be duplicated."));
        refreshRecentSessions();
    };
    addAndMakeVisible(recentPanel);
    addAndMakeVisible(trendPanel);
    // AUTO-5: built in the body (it captures the controller - ENG-2). It
    // measures files, so it runs on the long pool and only while the engine
    // is up and no track is being analysed.
    recurringPanel_ = std::make_unique<RecurringIssuesPanel>(
        [this](std::function<void()> job) { controller.runLongInBackground(std::move(job)); });
    recurringPanel_->canRunNow = [this] {
        // "Starting" can last while the engine already answers; a stopped one waits.
        const auto st = controller.lifecycle().getPythonServiceStatus();
        return st != PythonStatus::Stopped && st != PythonStatus::GaveUp &&
               !controller.analysis().resultsPending();
    };
    recurringPanel_->onOpenSession = [this](const std::string& id) {
        if (!controller.sessions().loadSessionById(id))
            controller.shell().showServiceError(TRANS("That session could not be opened."));
    };
    addAndMakeVisible(*recurringPanel_);

    createButton.setEnabled(false);
    createButton.onClick = [this] { onCreateClicked(); };
    button_role::markPrimary(createButton); // UX-4
    addAndMakeVisible(createButton);

    serviceStatusLabel_.setFont(khensu::KhensuTheme::uiFont(khensu::type::caption, juce::Font::plain));
    serviceStatusLabel_.setVisible(false);
    addChildComponent(serviceStatusLabel_);

    startServiceButton_.setVisible(false);
    startServiceButton_.onClick = [this] { controller.lifecycle().restartPythonService(); };
    addChildComponent(startServiceButton_);

    targetOverridesButton_.onClick = [this] {
        GenreEntry genre;
        if (targetPanel) {
            auto target = targetPanel->buildTarget();
            if (auto g = controller.genreDatabase().findById(target.primary_genre)) genre = *g;
        }
        auto* overlay = new TargetOverrideEditor(controller, genre);
        targetOverrideOverlay_.reset(overlay);
        overlay->onClose = [this] {
            targetOverrideOverlay_.reset();
            resized();
        };
        addAndMakeVisible(*targetOverrideOverlay_);
        targetOverrideOverlay_->setBounds(getLocalBounds());
    };
    addAndMakeVisible(targetOverridesButton_);

    importStemsButton_.setTooltip(TRANS("Import a folder of bounced stems as a multitrack session: per-track advice "
                                        "and track-vs-track masking."));
    importStemsButton_.onClick = [this] { openStemFolder(); };
    addAndMakeVisible(importStemsButton_);

    // UX-12: Advanced holds the pipeline controls; the detected context gets
    // one Confirm; the target form opens on demand.
    advancedButton_.setTooltip(TRANS("Genre source, reference track, stage retry and target overrides"));
    advancedButton_.onClick = [this] {
        showAdvanced_ = !showAdvanced_;
        updateDisclosure();
    };
    addAndMakeVisible(advancedButton_);
    confirmGenreButton_.setTooltip(TRANS("Use the detected genre for genre-relative targets and actions"));
    confirmGenreButton_.onClick = [this] {
        // NEW-9: the genre box only gets the suggestion once it is confirmed.
        if (targetPanel != nullptr && suggestedGenreId_.isNotEmpty())
            targetPanel->fillDetectedGenre(suggestedGenreId_);
        confirmCandidateToggle_.setToggleState(true, juce::dontSendNotification);
        if (!probedFiles_.empty()) refreshFullIntakeContext(probedFiles_[0].path);
        updateDisclosure();
    };
    addChildComponent(confirmGenreButton_);
    changeTargetButton_.setTooltip(TRANS("Set the genre, mood, energy and targets yourself"));
    changeTargetButton_.onClick = [this] {
        showTargetForm_ = !showTargetForm_;
        updateDisclosure();
    };
    addChildComponent(changeTargetButton_);
    detectedLabel_.setFont(KhensuTheme::bodyFont(type::large));
    detectedLabel_.setJustificationType(juce::Justification::topLeft);
    addChildComponent(detectedLabel_);
    createHintLabel_.setText(TRANS("Create Session unlocks when the first file is analysed."),
                             juce::dontSendNotification);
    createHintLabel_.setFont(KhensuTheme::bodyFont(type::secondary));
    createHintLabel_.setJustificationType(juce::Justification::centredRight);
    addChildComponent(createHintLabel_);
    confirmCandidateToggle_.onStateChange = [this] { updateDetectedLabel(); };
    updateDisclosure();

    refreshRecentSessions();
}

void IntakeScreen::openStemFolder()
{
    stemFolderChooser_ = std::make_unique<juce::FileChooser>(TRANS("Choose a folder of bounced stems"), juce::File(),
                                                             juce::String(), true);
    juce::Component::SafePointer<IntakeScreen> self(this);
    stemFolderChooser_->launchAsync(
        juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
        [self](const juce::FileChooser& chooser) {
            if (self == nullptr) return;
            const auto folder = chooser.getResult();
            if (!folder.isDirectory()) return;
            auto* view = new MultitrackSessionView(self->controller, folder);
            self->multitrackOverlay_.reset(view);
            // Deferred: the overlay is deleted from inside its own button callback.
            auto closeLater = [self] {
                juce::MessageManager::callAsync([self] {
                    if (self == nullptr) return;
                    self->multitrackOverlay_.reset();
                    self->resized();
                });
            };
            view->onClose = closeLater;
            view->onCreateSession = [self, closeLater](const juce::String& name, const std::vector<AudioFileRef>& files) {
                if (self == nullptr || files.empty()) return;
                const auto target = self->buildSessionTarget();
                if (!self->controller.sessions().startSession(name, files, target)) {
                    self->controller.shell().showServiceError( // UX-10
                        TRANS("Could not create the session: it could not be saved. Check the application data folder."));
                    return;
                }
                closeLater();
            };
            self->addAndMakeVisible(*view);
            view->setBounds(self->getLocalBounds());
        });
}

void IntakeScreen::retrySelectedIntakeStage()
{
    if (probedFiles_.empty()) return;
    const auto stage = selectedIntakeStage();
    if (stage.isEmpty()) return;
    const auto path = juce::String(probedFiles_[0].path);
    retryStageButton_.setEnabled(false);
    serviceStatusLabel_.setText(TRANS("Retrying intake stage:") + " " + stage, juce::dontSendNotification);
    serviceStatusLabel_.setVisible(true);
    resized();
    const auto settings = currentIntakeSettings();
    juce::Component::SafePointer<IntakeScreen> self(this);
    controller.runInBackground([self, path, stage, settings] {
        ipc::HttpServiceBridge bridge(PythonServiceManager::kBaseUrl, 60000, 0);
        const auto response = bridge.retryFullIntakeStage(path, stage, settings.statedGenre, settings.reference,
                                                          settings.selection, settings.useTargets,
                                                          settings.confirmDetected);
        juce::MessageManager::callAsync([self, response, path] {
            if (self == nullptr) return;
            self->retryStageButton_.setEnabled(true);
            // Another file was dropped while the stage ran (FIX-6): this is
            // the previous file's stage map, and refreshing the context for
            // `path` would put its findings under the new file.
            if (self->probedFiles_.empty() || juce::String(self->probedFiles_[0].path) != path) return;
            // The retry route returns the one stage's status ({"ok": ...}), not a map.
            const juce::var stageStatus = response[af::FullIntakeRetryResponse::stage_status];
            self->intakeStageStatus_ = response[af::FullIntakeRetryResponse::bundle][af::FullIntakeBundle::stage_status];
            self->intakeStageResults_ = response[af::FullIntakeRetryResponse::bundle][af::FullIntakeBundle::results];
            self->showSelectedStageStatus();
            self->serviceStatusLabel_.setVisible(true);
            self->resized();
            const bool ok = stageStatus.isObject() && (bool)stageStatus[af::IntakeStageOutcome::ok];
            if (ok) self->refreshFullIntakeContext(path);
            self->resized();
        });
    });
}

void IntakeScreen::showSelectedStageStatus()
{
    const auto stage = selectedIntakeStage();
    const auto status = intakeStageStatus_[juce::Identifier(stage)];
    if (!status.isObject()) return;
    const bool ok = bool(status[af::IntakeStageOutcome::ok]);
    const auto result = intakeStageResults_[juce::Identifier(stage)];
    juce::String text;
    if (!ok)
        text = TRANS("Intake stage failed:") + " " + stage + " - " + status[af::IntakeStageOutcome::error].toString();
    else if (result.isObject() && bool(result[af::IntakeStemContext::deferred]))
        text = TRANS("Intake stage deferred:") + " " + stage + " - " + result[af::IntakeStemContext::reason].toString() +
               " Open the stem workspace after separation completes.";
    else
        text = TRANS("Intake stage completed:") + " " + stage;
    serviceStatusLabel_.setText(text, juce::dontSendNotification);
    serviceStatusLabel_.setVisible(true);
    resized();
}

void IntakeScreen::cancelFullIntakeJob()
{
    if (activeIntakeJobId_.isEmpty()) return;
    const auto jobId = activeIntakeJobId_;
    cancelIntakeButton_.setEnabled(false);
    serviceStatusLabel_.setText(TRANS("Cancelling intake..."), juce::dontSendNotification);
    juce::Component::SafePointer<IntakeScreen> self(this);
    controller.runInBackground([self, jobId] {
        ipc::HttpServiceBridge bridge(PythonServiceManager::kBaseUrl, 5000, 0);
        const auto response = bridge.cancelFullIntake(jobId);
        juce::MessageManager::callAsync([self, response, jobId] {
            if (self == nullptr) return;
            // A new intake started while the cancel was in flight (FIX-6):
            // its id, its Cancel button and its status line are not this
            // job's to clear.
            if (self->activeIntakeJobId_ != jobId) return;
            self->activeIntakeJobId_.clear();
            self->cancelIntakeButton_.setEnabled(false);
            self->serviceStatusLabel_.setText(response.isObject() && bool(response[af::FullIntakeJobResponse::cancelled])
                                                  ? TRANS("Intake cancelled.")
                                                  : TRANS("Intake cancellation request failed."),
                                              juce::dontSendNotification);
        });
    });
}

void IntakeScreen::jumpToSelectedIntakeStage()
{
    const auto stage = selectedIntakeStage();
    if (stage == af::FullIntakeResults::artifacts)
        controller.shell().showScreen(ScreenType::AiDetection);
    else if (stage == af::FullIntakeResults::stem_context)
        controller.shell().showScreen(ScreenType::VocalWorkspace);
    else if (stage == af::FullIntakeResults::musical_context)
        controller.shell().showScreen(ScreenType::MixMaster);
    else
        // Technical preflight is summarized by the Dashboard analysis panels.
        controller.shell().showScreen(ScreenType::Dashboard);
}

IntakeScreen::~IntakeScreen() = default;

int IntakeScreen::queueIndexForPath(const juce::String& path) const
{
    for (size_t i = 0; i < probedFiles_.size(); ++i)
        if (juce::String(probedFiles_[i].path) == path) return (int)i;
    return -1;
}

void IntakeScreen::onFilesChosen(const std::vector<juce::File>& files)
{
    if (files.empty()) return;

    probedFiles_.clear();
    AudioFileProber prober;

    std::vector<IntakeQueueState::Entry> entries;
    for (const auto& file : files) {
        auto result = prober.probe(file.getFullPathName().toStdString());
        if (result.is_ok()) {
            probedFiles_.push_back(result.value());
            IntakeQueueState::Entry e;
            e.path = juce::String(probedFiles_.back().path);
            e.durationSeconds = probedFiles_.back().duration_seconds;
            entries.push_back(std::move(e));
        }
    }
    // DSK-01: a new generation - the previous drop's worker stops and its
    // in-flight analysis job is cancelled.
    const int generation = queueState_->reset(std::move(entries));

    if (probedFiles_.empty()) {
        queueList_.clear();
        nativeTiles_->clear();
        haveProbedFile = false;
        createButton.setEnabled(false);
        updateDisclosure();
        controller.shell().showServiceError( // UX-10
            TRANS("Unsupported files: could not read audio metadata from any of the selected files."));
        return;
    }

    haveProbedFile = true;
    // A new file: its own detection, confirmed (or not) on its own.
    detectedSummary_.clear();
    suggestedGenreId_.clear();
    suggestedGenreText_.clear();
    confirmCandidateToggle_.setToggleState(false, juce::dontSendNotification);
    queueList_.setFiles(probedFiles_);
    // CE-PLT-3: headline numbers now, from the native engine - the service
    // (possibly still starting) fills in everything else.
    nativeTiles_->measure(juce::File(juce::String(probedFiles_[0].path)));
    refreshFullIntakeContext(juce::String(probedFiles_[0].path));

    // Collapsed drop-zone summary for the first file.
    juce::String name = probedFiles_[0].title.empty() ? juce::File(probedFiles_[0].path).getFileName()
                                                      : juce::String(probedFiles_[0].title);
    if (probedFiles_.size() > 1) {
        name += " (+ " + juce::String(probedFiles_.size() - 1) + " " + TRANS("other files") + ")";
    }
    const auto& pf = probedFiles_[0];
    const int durSec = (int)std::lround(pf.duration_seconds);
    juce::String durStr = juce::String::formatted("%d:%02d", durSec / 60, durSec % 60);
    juce::String specStr = juce::String(pf.sample_rate / 1000.0, 1) + " kHz" +
                           (pf.channels == 1   ? " mono"
                            : pf.channels == 2 ? " stereo"
                                               : " " + juce::String(pf.channels) + "ch") +
                           (pf.bit_depth > 0 ? " / " + juce::String(pf.bit_depth) + "-bit" : juce::String());
    dropZone.setLoaded(name, juce::File(pf.path).getFileExtension().toUpperCase().trimCharactersAtStart("."), durStr,
                       specStr);
    resized();

    createButton.setEnabled(false);
    createButton.setButtonText(TRANS("Analyzing..."));
    updateDisclosure();

    serviceStatusLabel_.setVisible(false);

    resized();
    startServiceButton_.setVisible(false);
    resized();

    juce::Component::SafePointer<IntakeScreen> self(this);
    auto state = queueState_;
    const int totalFiles = (int)probedFiles_.size();
    // P0-5: the service's lifecycle status lives on the message thread; the
    // worker reads the last value published here (refreshed each wait step).
    auto serviceStatus =
        std::make_shared<std::atomic<int>>((int)controller.lifecycle().getPythonServiceStatus());
    // DSK-01: the worker touches ONLY `state` (mutex-guarded) and the bridge.
    // `self` is dereferenced exclusively inside callAsync on the message
    // thread, after a generation check - never on this worker.
    controller.runLongInBackground([self, state, generation, totalFiles, serviceStatus]() { // service wait + per-file job polls
        auto onMessageThread = [self, state, generation](std::function<void(IntakeScreen&)> fn) {
            juce::MessageManager::callAsync([self, state, generation, fn = std::move(fn)] {
                if (self == nullptr || !state->isCurrent(generation)) return; // a newer drop owns the screen
                fn(*self.getComponent());
            });
        };
        // Every exit path must reset the Create Session button and surface an
        // error so the screen never gets stuck on "Analyzing..." silently.
        try {
            ipc::HttpServiceBridge quickCheck(PythonServiceManager::kBaseUrl, 1000, 0);

            // P0-5: wait as long as the service process is alive and still
            // starting (a first-run install takes 1-3 min); the 15 s budget
            // only counts while nothing is running (ServiceWaitPolicy.h).
            bool serviceUp = false;
            bool gaveUpOnService = false;
            constexpr double kStepSeconds = 0.5;
            double secondsTotal = 0.0, secondsDown = 0.0;
            for (;;) {
                if (quickCheck.is_service_available()) {
                    serviceUp = true;
                    break;
                }
                if (!state->isCurrent(generation) || backgroundShouldStop()) return;
                const auto status = (PythonStatus)serviceStatus->load();
                if (service_wait::decide(status, secondsDown, secondsTotal) == service_wait::Decision::GiveUp) {
                    gaveUpOnService = status == PythonStatus::GaveUp;
                    break;
                }
                const bool starting = status == PythonStatus::Starting;
                const int shown = starting ? (int)secondsTotal : juce::jmax(0, (int)(service_wait::kDownSeconds - secondsDown));
                onMessageThread([starting, shown, serviceStatus](IntakeScreen& s) {
                    serviceStatus->store((int)s.controller.lifecycle().getPythonServiceStatus());
                    s.serviceStatusLabel_.setText(
                        starting ? TRANS("Setting up the analysis engine (the first run takes about 2 min)...") + " " +
                                       juce::String(shown) + " s"
                                 : TRANS("Waiting for local analysis service... retrying for") + " " +
                                       juce::String(shown) + " s",
                        juce::dontSendNotification);
                    s.serviceStatusLabel_.setVisible(true);
                    s.resized();
                });
                if (!backgroundSleep((int)(kStepSeconds * 1000))) return;
                secondsTotal += kStepSeconds;
                if (!starting) secondsDown += kStepSeconds;
            }

            if (!serviceUp) {
                onMessageThread([gaveUpOnService](IntakeScreen& s) {
                    s.createButton.setEnabled(true);
                    s.updateDisclosure();
                    s.createButton.setButtonText(TRANS("Create Session"));
                    s.serviceStatusLabel_.setText(
                        gaveUpOnService ? TRANS("The local analysis service could not be started.")
                                        : TRANS("Local analysis service did not respond."),
                        juce::dontSendNotification);
                    s.startServiceButton_.setVisible(true);
                    s.resized();
                });
                return;
            }

            onMessageThread([](IntakeScreen& s) {
                s.serviceStatusLabel_.setVisible(false);
                s.resized();
                s.startServiceButton_.setVisible(false);
                s.resized();
            });

            // Start + fallback calls (the bridge applies its own 30 s per-call
            // ceiling to the blocking /analyze fallback).
            ipc::HttpServiceBridge bridge(PythonServiceManager::kBaseUrl, 60000, 0);
            ipc::HttpServiceBridge pollBridge(PythonServiceManager::kBaseUrl, 10000, 0);

            // Intake queue: process the files sequentially in their CURRENT
            // order (read under the queue's mutex each step), honouring
            // per-file cancellation. The first successful analysis
            // auto-populates the target panel and unlocks Create Session.
            int failed = 0;
            int done = 0;
            int index = 0;
            bool first = true;
            while (auto entry = state->takeNext(generation)) {
                if (backgroundShouldStop()) return;
                const juce::String path = entry->path;
                const auto cancelFlag = entry->cancelled;
                const bool isFirst = first;
                first = false;
                ++index;

                onMessageThread([index, path, totalFiles](IntakeScreen& s) {
                    if (int idx = s.queueIndexForPath(path); idx >= 0)
                        s.queueList_.setStatus(idx, IntakeQueueList::Status::Analyzing);
                    if (totalFiles > 1) {
                        s.serviceStatusLabel_.setText(TRANS("Intake queue: analyzing") + " " + juce::String(index) +
                                                          "/" + juce::String(totalFiles) + " - " +
                                                          juce::File(path).getFileName(),
                                                      juce::dontSendNotification);
                        s.serviceStatusLabel_.setVisible(true);
                        s.resized();
                    }
                });

                khensu::Result<ipc::DeepAnalysisResponse, ipc::BridgeError> res =
                    khensu::Result<ipc::DeepAnalysisResponse, ipc::BridgeError>::err(
                        {ipc::BridgeError::Kind::ServiceUnavailable, "not attempted", 0});
                try {
                    // Job-poll path through the shared JobPoller: a cancel
                    // (row removed, newer drop) stops the wait at once and
                    // cancels the job server-side; the deadline scales with
                    // the file's duration instead of a fixed 120 s that long
                    // tracks never met.
                    JobPoller<ipc::AnalysisJobStatus>::Hooks hooks;
                    hooks.start = [&] {
                        return bridge.startAnalysisJob(path.toStdString(),
                                                       ipc::ILocalServiceBridge::AnalysisMode::Advanced);
                    };
                    hooks.poll = [&](const std::string& id) { return pollBridge.getAnalysisProgress(id); };
                    hooks.cancel = [&](const std::string& id) { pollBridge.cancelAnalysisJob(id); };
                    hooks.isTerminal = [](const ipc::AnalysisJobStatus& s) { return khensu::api::isTerminalStatus(s.status); };
                    hooks.shouldStop = [cancelFlag, state, generation] {
                        return cancelFlag->load() || !state->isCurrent(generation);
                    };
                    JobPoller<ipc::AnalysisJobStatus>::Options opts;
                    opts.pollIntervalMs = 150;
                    opts.maxConsecutiveErrors = 40; // ~6 s of failed polls
                    opts.deadlineSeconds = juce::jmax(180.0, entry->durationSeconds * 3.0);
                    const auto polled = JobPoller<ipc::AnalysisJobStatus>::run(hooks, opts);
                    if (polled.finished() && polled.status->result.has_value()) {
                        // The finished job carries its parsed result: no re-POST.
                        res = khensu::Result<ipc::DeepAnalysisResponse, ipc::BridgeError>::ok(*polled.status->result);
                    } else if (polled.finished() && polled.status->has_result) {
                        // Older service: the identical dict through analyze()'s parser.
                        res = bridge.analyze(path.toStdString(), ipc::ILocalServiceBridge::AnalysisMode::Advanced);
                    } else if (polled.outcome == JobPollOutcome::StartFailed) {
                        // Nothing is running server-side: the blocking call is
                        // the only remaining way to analyse this file.
                        res = bridge.analyze(path.toStdString(), ipc::ILocalServiceBridge::AnalysisMode::Advanced);
                    } else if (!polled.finished()) {
                        res = khensu::Result<ipc::DeepAnalysisResponse, ipc::BridgeError>::err(
                            {ipc::BridgeError::Kind::Timeout, polled.describe().toStdString(), 0});
                    }
                } catch (...) {
                    // One broken file must not take down the rest of the queue.
                }

                if (!state->isCurrent(generation)) return; // a newer drop took over
                if (cancelFlag->load()) {
                    onMessageThread([path](IntakeScreen& s) {
                        if (int idx = s.queueIndexForPath(path); idx >= 0)
                            s.queueList_.setStatus(idx, IntakeQueueList::Status::Cancelled);
                    });
                    continue;
                }

                if (res.is_err()) {
                    ++failed;
                    onMessageThread([path](IntakeScreen& s) {
                        if (int idx = s.queueIndexForPath(path); idx >= 0)
                            s.queueList_.setStatus(idx, IntakeQueueList::Status::Failed);
                    });
                } else {
                    ++done;
                    auto data = res.value();
                    onMessageThread([data, path, isFirst](IntakeScreen& s) mutable {
                        if (int idx = s.queueIndexForPath(path); idx >= 0)
                            s.queueList_.setStatus(idx, IntakeQueueList::Status::Done);
                        if (isFirst && s.targetPanel != nullptr) {
                            // NEW-9: no genre - the fast pass's decision tree named the
                            // wrong family more often than chance. The full intake's
                            // genre context offers a suggestion to confirm instead.
                            // TGT-1: nor the hook / payoff / heaviness / memorability
                            // targets - they are what the user wants, and the song's own
                            // scores there would make those findings judge it against itself.
                            s.targetPanel->autoPopulateFromAnalysis({}, {}, data.mood, data.energy, data.bpm,
                                                                    data.key);
                            // UX-12: what was detected, in one line, with one Confirm.
                            juce::StringArray parts;
                            if (juce::String(data.key).isNotEmpty()) parts.add(juce::String(data.key));
                            if (data.bpm > 0.0f) parts.add(juce::String(juce::roundToInt(data.bpm)) + " BPM");
                            s.detectedSummary_ = parts.joinIntoString("  -  ");
                            s.updateDetectedLabel();
                        }
                        s.controller.analysis().cacheIntakeAnalysis(path, std::move(data));
                    });
                }

                onMessageThread([](IntakeScreen& s) {
                    s.createButton.setEnabled(true);
                    s.createButton.setButtonText(TRANS("Create Session"));
                    s.updateDisclosure();
                });
            }

            const int cancelled = juce::jmax(0, totalFiles - done - failed);
            onMessageThread([failed, cancelled, totalFiles](IntakeScreen& s) {
                if (totalFiles > 1) {
                    s.serviceStatusLabel_.setText(formatIntakeQueueCompletion(totalFiles, failed, cancelled),
                                                  juce::dontSendNotification);
                    s.serviceStatusLabel_.setVisible(true);
                    s.resized();
                } else {
                    s.serviceStatusLabel_.setVisible(false);
                    s.resized();
                }
                // Everything cancelled: nothing unlocked Create Session above.
                if (s.haveProbedFile) {
                    s.createButton.setEnabled(true);
                    s.updateDisclosure();
                    s.createButton.setButtonText(TRANS("Create Session"));
                }
                s.resized();
            });
        } catch (const std::exception& e) {
            const juce::String message(e.what());
            onMessageThread([message](IntakeScreen& s) {
                s.createButton.setEnabled(true);
                s.updateDisclosure();
                s.createButton.setButtonText(TRANS("Create Session"));
                s.serviceStatusLabel_.setText(TRANS("Analysis failed:") + " " + message, juce::dontSendNotification);
                s.serviceStatusLabel_.setVisible(true);
                s.resized();
            });
        } catch (...) {
            onMessageThread([](IntakeScreen& s) {
                s.createButton.setEnabled(true);
                s.updateDisclosure();
                s.createButton.setButtonText(TRANS("Create Session"));
                s.serviceStatusLabel_.setText(TRANS("Analysis failed: unknown error."), juce::dontSendNotification);
                s.serviceStatusLabel_.setVisible(true);
                s.resized();
            });
        }
    });
}

IntakeScreen::IntakeSettings IntakeScreen::currentIntakeSettings() const
{
    const int source = genreSourceCombo_.getSelectedId();
    IntakeSettings st;
    st.selection = source == 2   ? juce::String("stated")
                   : source == 3 ? juce::String("reference")
                   : source == 4 ? juce::String("none")
                                 : juce::String("detected");
    st.useTargets = genreTargetsToggle_.getToggleState() && source != 4;
    st.confirmDetected = st.selection == "detected" && confirmCandidateToggle_.getToggleState();
    st.statedGenre = targetPanel ? juce::String(targetPanel->buildTarget().primary_genre) : juce::String();
    st.reference = referencePathEditor_.getText().trim();
    return st;
}

void IntakeScreen::chooseReferenceFile()
{
    referenceChooser_ = std::make_unique<juce::FileChooser>(
        TRANS("Choose a reference track"), juce::File(referencePathEditor_.getText().trim()).getParentDirectory(),
        "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg;*.m4a");
    juce::Component::SafePointer<IntakeScreen> self(this);
    referenceChooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                   [self](const juce::FileChooser& fc) {
                                       if (self == nullptr) return;
                                       const auto chosen = fc.getResult();
                                       if (!chosen.existsAsFile()) return;
                                       self->referencePathEditor_.setText(chosen.getFullPathName(), false);
                                       self->commitReferencePath();
                                   });
}

void IntakeScreen::commitReferencePath()
{
    const juce::String path = referencePathEditor_.getText().trim();
    if (path == committedReferencePath_) return;
    committedReferencePath_ = path;
    if (genreSourceCombo_.getSelectedId() == 3 && !probedFiles_.empty())
        refreshFullIntakeContext(probedFiles_[0].path);
}

void IntakeScreen::startMatchReference()
{
    showAdvanced_ = true; // the reference controls live under Advanced (UX-12)
    updateDisclosure();
    genreSourceCombo_.setSelectedId(3, juce::sendNotificationSync);
    if (referencePathEditor_.getText().trim().isEmpty()) chooseReferenceFile();
}

void IntakeScreen::presetGenre(const juce::String& genreId)
{
    if (targetPanel && genreId.isNotEmpty()) targetPanel->autoSelectGenre(genreId, {});
}

juce::String IntakeScreen::selectedIntakeStage() const
{
    const int index = intakeStageCombo_.getSelectedItemIndex();
    return index >= 0 && index < intakeStageKeys_.size() ? intakeStageKeys_[index] : juce::String();
}

void IntakeScreen::sendGenreExample(const CreativeTarget& target)
{
    // GEN-1e: a genre the user confirmed or chose for one track teaches the
    // genre suggestion their music. Fire and forget: without the probe the
    // service stores nothing, and a failure only means one example fewer.
    const auto source = intake_text::genreExampleSource(target, suggestedGenreId_);
    if (source.isEmpty() || probedFiles_.size() != 1) return;
    const juce::String path(probedFiles_[0].path);
    const juce::String genre(target.primary_genre);
    controller.runInBackground([path, genre, source] {
        ipc::HttpServiceBridge bridge(PythonServiceManager::kBaseUrl, 60000, 0);
        (void)bridge.addGenreExample(path, genre, source);
    });
}

CreativeTarget IntakeScreen::buildSessionTarget() const
{
    auto target = targetPanel ? targetPanel->buildTarget() : CreativeTarget{};
    const auto settings = currentIntakeSettings();
    const bool referenceExists = settings.reference.isNotEmpty() && juce::File::isAbsolutePath(settings.reference) &&
                                 juce::File(settings.reference).existsAsFile();
    genre_confirmation::IntakeChoice choice;
    choice.selectionMode = settings.selection.toStdString();
    choice.useGenreTargets = settings.useTargets;
    // The Target panel's genre counts as stated only if the user picked it:
    // the first analysis fills it with the detected genre, which must be
    // confirmed first (UX-12; it used to count as stated, so an unconfirmed
    // detection unlocked genre-relative actions).
    const bool genreFromUser = targetPanel != nullptr && targetPanel->genreStatedByUser();
    choice.statedGenreId = settings.selection == "stated" || genreFromUser ? target.primary_genre : std::string();
    choice.referenceExists = referenceExists;
    choice.confirmDetected = settings.confirmDetected;
    target.use_genre_targets = settings.useTargets;
    target.genre_confirmed_source = genre_confirmation::sourceForIntake(choice);
    if (referenceExists) target.reference_track_paths = {settings.reference.toStdString()};
    return target;
}

void IntakeScreen::refreshFullIntakeContext(const juce::String& path)
{
    const auto settings = currentIntakeSettings();
    const auto& selection = settings.selection;
    const bool useTargets = settings.useTargets;
    const bool confirmDetected = settings.confirmDetected;
    const auto& statedGenre = settings.statedGenre;
    const auto& reference = settings.reference;

    // DSK-18: a new drop (or settings change) supersedes the running job -
    // cancel it server-side so it stops computing, and ignore its late bundle.
    if (activeIntakeJobId_.isNotEmpty()) {
        const auto oldJob = activeIntakeJobId_;
        controller.runInBackground([oldJob] {
            ipc::HttpServiceBridge bridge(PythonServiceManager::kBaseUrl, 5000, 0);
            bridge.cancelFullIntake(oldJob);
        });
        activeIntakeJobId_.clear();
        cancelIntakeButton_.setEnabled(false);
    }
    const int generation = ++fullIntakeGeneration_;
    fullIntakePath_ = path;
    double durationSeconds = 0.0;
    for (const auto& f : probedFiles_)
        if (juce::String(f.path) == path) durationSeconds = f.duration_seconds;

    juce::Component::SafePointer<IntakeScreen> self(this);
    // `self` is only dereferenced on the message thread, after these checks.
    auto stillCurrent = [self, generation, path]() {
        return self != nullptr && self->fullIntakeGeneration_ == generation && self->fullIntakePath_ == path;
    };
    controller.runLongInBackground([self, stillCurrent, path, selection, useTargets, reference, statedGenre,
                                    confirmDetected, durationSeconds] {
        ipc::HttpServiceBridge bridge(PythonServiceManager::kBaseUrl, 60000, 0);
        ipc::HttpServiceBridge pollBridge(PythonServiceManager::kBaseUrl, 10000, 0);

        // WIRE-02: a DURATION-based deadline (the old 120 x 250 ms = 30 s gave
        // up on any real track), a server-side cancel on timeout, and never a
        // synchronous re-run of the whole intake while the job still runs -
        // that re-POST did not join the running job, it ran a second one.
        JobPoller<juce::var>::Hooks hooks;
        hooks.start = [&]() -> Result<std::string, ipc::BridgeError> {
            const auto started =
                bridge.startFullIntake(path, statedGenre, reference, selection, useTargets, confirmDetected);
            const auto jobId = started[af::FullIntakeJobResponse::job_id].toString();
            if (jobId.isEmpty())
                return Result<std::string, ipc::BridgeError>::err(
                    {ipc::BridgeError::Kind::ServiceUnavailable, "full intake could not start", 0});
            juce::MessageManager::callAsync([self, stillCurrent, jobId] {
                if (!stillCurrent()) return;
                self->activeIntakeJobId_ = jobId;
                self->cancelIntakeButton_.setEnabled(true);
            });
            return Result<std::string, ipc::BridgeError>::ok(jobId.toStdString());
        };
        hooks.poll = [&](const std::string& id) -> Result<juce::var, ipc::BridgeError> {
            const auto status = pollBridge.fullIntakeStatus(juce::String(id));
            if (!status.isObject())
                return Result<juce::var, ipc::BridgeError>::err(
                    {ipc::BridgeError::Kind::ServiceUnavailable, "no intake status", 0});
            return Result<juce::var, ipc::BridgeError>::ok(status);
        };
        hooks.cancel = [&](const std::string& id) { pollBridge.cancelFullIntake(juce::String(id)); };
        hooks.isTerminal = [](const juce::var& s) {
            return khensu::api::isTerminalStatus(s[af::FullIntakeJobResponse::state].toString());
        };
        JobPoller<juce::var>::Options opts;
        opts.pollIntervalMs = 500;
        opts.maxConsecutiveErrors = 20;
        // The plugin waits 10 minutes (CopilotServiceJobs); long tracks get
        // proportionally longer.
        opts.deadlineSeconds = juce::jmax(600.0, durationSeconds * 4.0);

        // Superseded by a newer drop: the next progress callback (on the
        // message thread - no IntakeScreen member is read here) flips the
        // flag, the poller stops and cancels the job server-side.
        auto supersededFlag = std::make_shared<std::atomic<bool>>(false);
        hooks.shouldStop = [supersededFlag] { return supersededFlag->load(); };
        hooks.onProgress = [self, stillCurrent, supersededFlag](const juce::var& status) {
            const auto stage = status[af::FullIntakeJobResponse::stage].toString();
            const auto state = status[af::FullIntakeJobResponse::state].toString();
            juce::MessageManager::callAsync([self, stillCurrent, supersededFlag, stage, state] {
                if (!stillCurrent()) {
                    supersededFlag->store(true);
                    return;
                }
                if (stage.isNotEmpty())
                    self->serviceStatusLabel_.setText(TRANS("Intake") + " " + state + ": " + stage,
                                                      juce::dontSendNotification);
            });
        };

        const auto polled = JobPoller<juce::var>::run(hooks, opts);

        juce::var bundle;
        juce::String endMessage;
        if (polled.finished()) {
            const auto state = (*polled.status)[af::FullIntakeJobResponse::state].toString();
            if (state == khensu::api::job_status::completed) {
                bundle = (*polled.status)[af::FullIntakeJobResponse::result];
            } else {
                const auto err = (*polled.status)[af::FullIntakeJobResponse::error].toString();
                endMessage = TRANS("Intake") + " " + state + (err.isNotEmpty() ? ": " + err : juce::String());
            }
        } else if (polled.outcome == JobPollOutcome::StartFailed) {
            // No job exists, so the synchronous route cannot duplicate one.
            bundle = bridge.fetchFullIntake(path, statedGenre, reference, selection, useTargets, confirmDetected);
        } else if (polled.outcome != JobPollOutcome::Stopped) {
            endMessage = polled.describe();
        }

        juce::MessageManager::callAsync([self, stillCurrent, bundle, endMessage, stopped = polled.outcome ==
                                                                                       JobPollOutcome::Stopped] {
            if (!stillCurrent()) return; // DSK-18: a late bundle for an older drop / another file
            self->activeIntakeJobId_.clear();
            self->cancelIntakeButton_.setEnabled(false);
            if (stopped) return;
            if (endMessage.isNotEmpty()) {
                self->genreContextLabel_.setText(endMessage + " - " +
                                                     TRANS("genre context unavailable; processing remains manual."),
                                                 juce::dontSendNotification);
                return;
            }
            auto context = bundle[af::FullIntakeBundle::results][af::FullIntakeResults::musical_context];
            self->intakeStageStatus_ = bundle[af::FullIntakeBundle::stage_status];
            self->intakeStageResults_ = bundle[af::FullIntakeBundle::results];
            self->showSelectedStageStatus();
            if (!context.isObject()) {
                self->genreContextLabel_.setText(TRANS("Genre context unavailable; processing remains manual."),
                                                 juce::dontSendNotification);
                return;
            }
            const auto suggestion = intake_text::genreSuggestion(context);
            self->suggestedGenreId_ = suggestion.genreId;
            self->suggestedGenreText_ = suggestion.text;
            self->updateDetectedLabel();
            juce::String text = TRANS("Candidates:") + " ";
            auto candidates = context[af::GenreContextResponse::candidates];
            juce::StringArray names;
            for (int i = 0; i < juce::jmin(5, candidates.size()); ++i)
                names.add(intake_text::candidateName(candidates[i]));
            text += names.joinIntoString(", ");
            text += "\n" + TRANS("Evidence:") + " " +
                    intake_text::formatGenreEvidence(context[af::GenreContextResponse::evidence]);
            auto sections = context[af::GenreContextResponse::section_candidates];
            if (context[af::GenreContextResponse::is_hybrid_or_conflicted])
                text += "\n" + TRANS("Hybrid/crossover: sections disagree - choose a source before targets are used.");
            else if (sections.isArray()) {
                text += "\n" + TRANS("Sections:") + " ";
                juce::StringArray rows;
                for (int i = 0; i < sections.size(); ++i) {
                    const auto row = sections[i];
                    rows.add(juce::String(row[af::GenreSectionCandidate::start_sec].toString()) + "-" + juce::String(row[af::GenreSectionCandidate::end_sec].toString()) +
                             "s: " + row[af::GenreSectionCandidate::top_candidate].toString());
                }
                text += rows.joinIntoString(" | ");
            }
            if (!bool(context[af::GenreContextResponse::genre_relative_action_allowed]))
                text += "\n" + TRANS("Needs confirmation before genre-relative settings are used.");
            self->genreContextLabel_.setText(text, juce::dontSendNotification);
            self->resized();
        });
    });
}

void IntakeScreen::onQueueReorder(int oldIndex, int newIndex)
{
    if (oldIndex < 0 || oldIndex >= (int)probedFiles_.size()) return;
    if (newIndex < 0 || newIndex >= (int)probedFiles_.size()) return;
    if (oldIndex == newIndex) return;

    auto file = probedFiles_[(size_t)oldIndex];
    probedFiles_.erase(probedFiles_.begin() + oldIndex);
    probedFiles_.insert(probedFiles_.begin() + newIndex, std::move(file));

    queueList_.setFilesAndPreserveStatus(probedFiles_);
    queueList_.resized();
    queueList_.repaint();

    // DSK-01: mirror the new order into the worker's guarded snapshot.
    std::vector<juce::String> order;
    for (const auto& f : probedFiles_)
        order.push_back(juce::String(f.path));
    queueState_->reorder(order);
}

void IntakeScreen::onQueueCancel(int index)
{
    if (index < 0 || index >= (int)probedFiles_.size()) return;
    const juce::String path(probedFiles_[(size_t)index].path);

    queueState_->cancel(path); // sets its cancel flag: an in-flight job is cancelled server-side

    probedFiles_.erase(probedFiles_.begin() + index);
    queueList_.setFilesAndPreserveStatus(probedFiles_);

    if (probedFiles_.empty()) {
        haveProbedFile = false;
        dropZone.clear();
        createButton.setEnabled(false);
    }
    updateDisclosure();
}

void IntakeScreen::onCreateClicked()
{
    if (!haveProbedFile || probedFiles_.empty()) return;

    juce::File f(probedFiles_[0].path);
    auto name = f.getFileNameWithoutExtension();

    const auto target = buildSessionTarget();
    if (probedFiles_.size() > 1) {
        // WF-7: one session per file - an averaged "Batch" session analysed
        // the files as one and used only the first downstream. The first
        // opens; the others wait in Recent sessions.
        const int created = controller.sessions().startSessionPerFile(probedFiles_, target);
        if (created == 0) {
            controller.shell().showServiceError( // UX-10
                TRANS("Could not create the sessions: they could not be saved. Check the application data folder."));
            return;
        }
        name = TRANS("%n sessions, one per file").replace("%n", juce::String(created));
    } else if (!controller.sessions().startSession(name, probedFiles_, target)) {
        controller.shell().showServiceError( // UX-10
            TRANS("Could not create the session: it could not be saved. Check the application data folder."));
        return;
    }
    sendGenreExample(target);

    // The intake report for a multi-file drop: each file's scan, each its own
    // session now (WF-7).
    if (probedFiles_.size() > 1) {
        std::vector<IntakeBatchReport::Item> items;
        for (int i = 0; i < (int)probedFiles_.size(); ++i) {
            const auto status = queueList_.getStatus(i);
            juce::String text = TRANS("Pending");
            switch (status) {
            case IntakeQueueList::Status::Done:
                text = TRANS("Analyzed");
                break;
            case IntakeQueueList::Status::Failed:
                text = TRANS("Failed");
                break;
            case IntakeQueueList::Status::Cancelled:
                text = TRANS("Cancelled");
                break;
            case IntakeQueueList::Status::Analyzing:
                text = TRANS("Analyzing");
                break;
            default:
                text = TRANS("Pending");
                break;
            }
            items.push_back({probedFiles_[(size_t)i], text});
        }
        batchReport_.reset(new IntakeBatchReport(name, items));
        batchReport_->onClose = [this] {
            batchReport_.reset();
            resized();
        };
        addAndMakeVisible(*batchReport_);
        batchReport_->setBounds(getLocalBounds());
    }
}

void IntakeScreen::refreshRecentSessions()
{
    // DSK-20: both reads off the message thread (WAL + busy_timeout: a
    // writer in the service stalled the UI each time this screen showed).
    auto sm = controller.sessions().sessionManagerPtr();
    if (sm == nullptr) return;
    juce::Component::SafePointer<IntakeScreen> self(this);
    controller.runInBackground([sm, self] {
        auto sessions = sm->listSessions(10);
        // AUTO-5: enough history for a trend (one row per session).
        auto summaries = sm->listRecentAnalysisSummaries(24);
        const bool sessionsFailed = sessions.is_err();
        std::vector<Session> sessionList = sessions.is_ok() ? sessions.value() : std::vector<Session>{};
        std::vector<SessionManager::AnalysisSummary> summaryList =
            summaries.is_ok() ? trend_rows::forChart(summaries.value()) : std::vector<SessionManager::AnalysisSummary>{};
        juce::MessageManager::callAsync([self, sessionList, summaryList, sessionsFailed] {
            if (self == nullptr) return;
            self->recentPanel.setSessions(sessionList);
            self->recentPanel.setLoadError(sessionsFailed);
            self->trendPanel.setSummaries(summaryList);
            if (self->targetPanel)
                self->trendPanel.setCurrentGenre(juce::String(self->targetPanel->buildTarget().primary_genre));
        });
    });
}

void IntakeScreen::visibilityChanged()
{
    if (isVisible()) refreshRecentSessions();
}

void IntakeScreen::paint(juce::Graphics& g) { KhensuLookAndFeel::drawGlassmorphicBackground(g, getLocalBounds()); }

void IntakeScreen::updateDetectedLabel()
{
    juce::String text;
    if (detectedSummary_.isEmpty())
        text = TRANS("Analysing the first file...");
    else {
        text = TRANS("Detected:") + " " + detectedSummary_ + "\n";
        const int source = genreSourceCombo_.getSelectedId();
        const bool stated = targetPanel != nullptr && targetPanel->genreStatedByUser();
        // NEW-9: the genre is offered, not detected - with how sure it is.
        if (source == 1 && !stated && genreTargetsToggle_.getToggleState())
            text += (suggestedGenreText_.isNotEmpty() ? suggestedGenreText_ : TRANS("Genre: identifying...")) + "\n";
        if (source == 4 || !genreTargetsToggle_.getToggleState())
            text += TRANS("Genre targets are off for this session.");
        else if (source == 2 || (targetPanel != nullptr && targetPanel->genreStatedByUser()))
            text += TRANS("Using the genre you chose.");
        else if (source == 3)
            text += TRANS("Using the reference track for targets.");
        else if (confirmCandidateToggle_.getToggleState())
            text += TRANS("Genre confirmed - genre-relative targets and actions are on.");
        else
            text += TRANS("Confirm the genre to use genre-relative targets, or change it.");
    }
    detectedLabel_.setText(text, juce::dontSendNotification);
    confirmGenreButton_.setVisible(haveProbedFile && detectedSummary_.isNotEmpty() && suggestedGenreId_.isNotEmpty() &&
                                   genreSourceCombo_.getSelectedId() == 1 && genreTargetsToggle_.getToggleState() &&
                                   !confirmCandidateToggle_.getToggleState() &&
                                   !(targetPanel != nullptr && targetPanel->genreStatedByUser()));
}

void IntakeScreen::updateDisclosure()
{
    const bool loaded = haveProbedFile;
    const bool advanced = showAdvanced_;
    for (juce::Component* c : std::initializer_list<juce::Component*>{
             &genreSourceCombo_, &genreTargetsToggle_, &confirmCandidateToggle_, &referencePathEditor_,
             &chooseReferenceButton_, &intakeStageCombo_, &retryStageButton_, &openStagePanelButton_,
             &cancelIntakeButton_, &targetOverridesButton_, &genreContextLabel_})
        c->setVisible(advanced);
    advancedButton_.setButtonText(advanced ? TRANS("Hide advanced") : TRANS("Advanced"));

    detectedLabel_.setVisible(loaded);
    changeTargetButton_.setVisible(loaded);
    changeTargetButton_.setButtonText(showTargetForm_ ? TRANS("Hide target") : TRANS("Change target..."));
    if (targetPanel) targetPanel->setVisible(loaded && showTargetForm_);
    createButton.setVisible(loaded);
    createHintLabel_.setVisible(loaded && !createButton.isEnabled());
    updateDetectedLabel();
    resized();
}

void IntakeScreen::resized()
{
    auto b = getLocalBounds().reduced(KhensuTheme::panelPadding);
    const bool loaded = haveProbedFile;

    // Screen header: Open (its action), Import stems, Advanced.
    auto header = b.removeFromTop(KhensuTheme::headerH);
    advancedButton_.setBounds(header.removeFromRight(140).reduced(4, 10));
    importStemsButton_.setBounds(header.removeFromRight(170).reduced(4, 10));
    titleLabel.setBounds(header);
    b.removeFromTop(KhensuTheme::cardGap);

    if (targetOverrideOverlay_) targetOverrideOverlay_->setBounds(getLocalBounds());
    if (multitrackOverlay_) multitrackOverlay_->setBounds(getLocalBounds());

    // Bottom: recent sessions strip (160px)
    auto recentStrip = b.removeFromBottom(160);
    recentPanel.setBounds(recentStrip);
    b.removeFromBottom(KhensuTheme::cardGap);

    // Button row: Create Session once a file is in, and the service status
    // (with Start Service) whenever it has something to say.
    if (loaded || serviceStatusLabel_.isVisible()) {
        auto btnRow = b.removeFromBottom(48);
        if (loaded) {
            createButton.setBounds(btnRow.removeFromRight(200).reduced(0, 6));
            btnRow.removeFromRight(8);
        }
        if (startServiceButton_.isVisible()) {
            startServiceButton_.setBounds(btnRow.removeFromRight(120).reduced(4, 8));
            btnRow.removeFromRight(4);
        }
        if (serviceStatusLabel_.isVisible())
            serviceStatusLabel_.setBounds(btnRow);
        else
            createHintLabel_.setBounds(btnRow);
        b.removeFromBottom(KhensuTheme::cardGap);
    }

    // Advanced: the pipeline controls.
    if (showAdvanced_) {
        auto contextRow = b.removeFromBottom(140);
        {
            auto top = contextRow.removeFromTop(26);
            targetOverridesButton_.setBounds(top.removeFromRight(150));
            top.removeFromRight(4);
            genreSourceCombo_.setBounds(top);
        }
        contextRow.removeFromTop(4);
        {
            auto refRow = contextRow.removeFromTop(24);
            chooseReferenceButton_.setBounds(refRow.removeFromRight(90));
            refRow.removeFromRight(4);
            referencePathEditor_.setBounds(refRow);
        }
        genreTargetsToggle_.setBounds(contextRow.removeFromTop(24));
        confirmCandidateToggle_.setBounds(contextRow.removeFromTop(24));
        auto retryRow = contextRow.removeFromTop(24);
        cancelIntakeButton_.setBounds(retryRow.removeFromRight(120));
        retryRow.removeFromRight(4);
        openStagePanelButton_.setBounds(retryRow.removeFromRight(92));
        retryStageButton_.setBounds(retryRow.removeFromRight(100));
        intakeStageCombo_.setBounds(retryRow);
        b.removeFromBottom(KhensuTheme::cardGap);
    }

    if (!loaded) {
        // Before a file: the drop zone, and below it the cross-session trends
        // beside the findings that recur across those sessions (AUTO-5).
        auto trendStrip = b.removeFromBottom(juce::jlimit(150, 220, b.getHeight() * 34 / 100));
        if (recurringPanel_ != nullptr) {
            recurringPanel_->setVisible(true);
            auto right = trendStrip.removeFromRight(trendStrip.getWidth() * 45 / 100);
            right.removeFromLeft(KhensuTheme::cardGap);
            recurringPanel_->setBounds(right);
        }
        trendPanel.setBounds(trendStrip);
        b.removeFromBottom(KhensuTheme::cardGap);
        dropZone.setBounds(b);
        nativeTiles_->setBounds({});
        queueList_.setBounds({});
        if (showAdvanced_) genreContextLabel_.setBounds({});
        return;
    }

    // After a drop: the file and its queue on the left, what was detected on
    // the right (with the target form when opened).
    if (recurringPanel_ != nullptr) recurringPanel_->setVisible(false); // the file's own details take the room
    auto left = b.removeFromLeft(b.getWidth() * 55 / 100);
    left.removeFromRight(KhensuTheme::cardGap);
    auto trendStrip = left.removeFromBottom(juce::jlimit(120, 200, left.getHeight() * 35 / 100));
    trendPanel.setBounds(trendStrip);
    left.removeFromBottom(KhensuTheme::cardGap);
    dropZone.setBounds(left.removeFromTop(60));
    left.removeFromTop(KhensuTheme::cardGap);
    nativeTiles_->setBounds(left.removeFromTop(64));
    left.removeFromTop(KhensuTheme::cardGap);
    queueList_.setBounds(left);

    // Detected key/BPM, the genre suggestion (may wrap) and the status line.
    detectedLabel_.setBounds(b.removeFromTop(84).reduced(2, 0));
    {
        auto row = b.removeFromTop(KhensuTheme::buttonH);
        if (confirmGenreButton_.isVisible()) {
            confirmGenreButton_.setBounds(row.removeFromLeft(150));
            row.removeFromLeft(8);
        }
        changeTargetButton_.setBounds(row.removeFromLeft(150));
    }
    b.removeFromTop(KhensuTheme::cardGap);
    if (showAdvanced_) {
        genreContextLabel_.setBounds(b.removeFromTop(56).reduced(2, 0));
        b.removeFromTop(KhensuTheme::cardGap);
    }
    if (targetPanel) targetPanel->setBounds(showTargetForm_ ? b : juce::Rectangle<int>());
}

} // namespace khensu
