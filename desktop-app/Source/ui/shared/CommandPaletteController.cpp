// desktop-app/Source/ui/shared/CommandPaletteController.cpp
#include "CommandPaletteController.h"

namespace khensu {

CommandPaletteController::CommandPaletteController(ICommandPaletteHost& host, juce::Component& parent)
    : host_(host), parent_(parent)
{
}

void CommandPaletteController::toggle()
{
    if (activeCommandPalette_ != nullptr)
        hide();
    else
        show();
}

void CommandPaletteController::show()
{
    if (activeCommandPalette_ != nullptr) return; // already visible

    buildCommands();
    activeCommandPalette_ = std::make_unique<CommandPaletteOverlay>(std::move(commands_));
    activeCommandPalette_->onDismiss = [this]() { hide(); };
    if (auto* app = host_.commandAppController())
        activeCommandPalette_->enableFreeTextCommand(
            [this, app](const juce::String& text) { showNlCommand(*app, text); });
    parent_.addAndMakeVisible(activeCommandPalette_.get());
    activeCommandPalette_->setBounds(parent_.getLocalBounds());
    activeCommandPalette_->toFront(true);
    activeCommandPalette_->grabKeyboardFocus();
}

void CommandPaletteController::hide()
{
    if (activeCommandPalette_ == nullptr) return;

    parent_.removeChildComponent(activeCommandPalette_.get());
    activeCommandPalette_.reset();
    parent_.grabKeyboardFocus(); // return focus so subsequent shortcuts route here
}

void CommandPaletteController::resized(const juce::Rectangle<int>& bounds)
{
    if (activeCommandPalette_ != nullptr) activeCommandPalette_->setBounds(bounds);
    if (activeNlCommand_ != nullptr) activeNlCommand_->setBounds(bounds);
}

void CommandPaletteController::showNlCommand(IAppController& app, const juce::String& text)
{
    if (activeNlCommand_ != nullptr) parent_.removeChildComponent(activeNlCommand_.get());
    activeNlCommand_ = std::make_unique<NlCommandOverlay>(app, text);
    activeNlCommand_->onDismiss = [this]() {
        // Deferred: the overlay's own Close button is mid-click right now.
        // The parent owns this controller, so a live parent means a live `this`.
        juce::Component::SafePointer<juce::Component> parent(&parent_);
        juce::MessageManager::callAsync([this, parent, done = activeNlCommand_.get()]() {
            if (parent == nullptr) return;
            if (activeNlCommand_ == nullptr || activeNlCommand_.get() != done) return;
            parent_.removeChildComponent(activeNlCommand_.get());
            activeNlCommand_.reset();
            parent_.grabKeyboardFocus();
        });
    };
    parent_.addAndMakeVisible(activeNlCommand_.get());
    activeNlCommand_->setBounds(parent_.getLocalBounds());
    activeNlCommand_->toFront(true);
}

bool CommandPaletteController::handleKeyPress(const juce::KeyPress& key)
{
    if ((key.getModifiers().isCommandDown() || key.getModifiers().isCtrlDown()) &&
        (key.getKeyCode() == 'K' || key.getKeyCode() == 'k')) {
        toggle();
        return true;
    }
    return false;
}

void CommandPaletteController::buildCommands()
{
    using Command = CommandPaletteOverlay::Command;
    commands_.clear();

    // Navigation - one per sidebar entry, in visual order.
    for (const auto& [label, action] : host_.navigationCommands()) {
        commands_.push_back({TRANS("Go to") + " " + label, "Go", label, action});
    }

    // UX-11: the core verbs, with their shortcut in the label.
    using Verb = ICommandPaletteHost::CoreVerb;
    const auto verb = [this](Verb v) { return [this, v]() { host_.runCoreVerb(v); }; };
    commands_.push_back({TRANS("Open audio file...") + "  (Ctrl+O)", "Do",
                         "load import browse track mix new session", verb(Verb::OpenFile)});
    commands_.push_back({TRANS("Analyse again") + "  (Ctrl+R)", "Do", "reanalyse reanalyze rerun scan refresh",
                         verb(Verb::Analyse)});
    commands_.push_back({TRANS("Fix top issue") + "  (F)", "Do", "guided wizard repair recommendation render",
                         verb(Verb::FixTopIssue)});
    commands_.push_back({TRANS("Toggle A/B (original / fix)") + "  (B)", "Do", "compare ab before after render",
                         verb(Verb::ToggleAb)});
    commands_.push_back({TRANS("Export") + "  (Ctrl+E)", "Go", "deliver bounce master platform render",
                         verb(Verb::Export)});
    commands_.push_back({TRANS("Settings") + "  (Ctrl+,)", "Go", "preferences options privacy online",
                         verb(Verb::Settings)});

    // Actions - always safe to invoke; the underlying methods self-guard when
    // there's no session/analysis/undo state.
    commands_.push_back({TRANS("Undo"), "Do", "revert back", [this]() {
                            if (host_.canUndo()) host_.undo();
                        }});
    commands_.push_back({TRANS("Redo"), "Do", "forward", [this]() {
                            if (host_.canRedo()) host_.redo();
                        }});
    commands_.push_back({TRANS("Next unresolved issue") + "  (N)", "Do", "problem recommendation fix",
                         [this]() { host_.focusNextIssue(); }});
    commands_.push_back({TRANS("Toggle report mode (Client / Engineer)"), "Do", "detail summary",
                         [this]() { host_.toggleReportMode(); }});
    commands_.push_back({TRANS("Save snapshot"), "Do", "checkpoint version history", [this]() {
                            if (host_.hasAnalysis()) host_.saveNamedSnapshot("Quick snapshot");
                        }});
    commands_.push_back({TRANS("Show or hide the session health strip"), "Do",
                         "break fatigue ears timer coach session health hud",
                         [this]() { host_.toggleSessionHealthStrip(); }});
    commands_.push_back({TRANS("Check for updates"), "Do", "new version release upgrade download installer github",
                         [this]() { host_.checkForAppUpdates(); }});
    commands_.push_back({TRANS("Cycle theme (Dark / High Contrast / Light)"), "Do",
                         "appearance colour accessibility contrast light dark",
                         [this]() { host_.cycleTheme(); }});
    commands_.push_back({TRANS("Set up a goal and starting chain (onboarding)"), "Do",
                         "onboarding wizard goal target genre platform chain setup first run",
                         [this]() { host_.showOnboardingWizard(); }});
    commands_.push_back({TRANS("Show welcome tour"), "Do", "help explainer onboarding", [this]() {
                            host_.showExplainer(TRANS("Welcome to Khensu"), TRANS("Tour"),
                                                TRANS("Press Ctrl+K anytime to jump between screens or run an action."));
                        }});
    commands_.push_back({TRANS("Open reference-track ritual"), "Do", "reference match eq tonal delta compare",
                         [this]() { host_.showReferenceTrackRitual(); }});
    if (auto* app = host_.commandAppController()) // CE-AG-4 push-to-talk
        commands_.push_back({TRANS("Voice mix instruction (push-to-talk)"), "Do",
                             "speak say talk microphone mic voice command instruction",
                             [this, app]() { showNlCommand(*app, {}); }});
    commands_.push_back({TRANS("Copy service log to clipboard"), "Do",
                         "python service debug diagnose log watchdog crash",
                         [this]() { host_.copyServiceLogsToClipboard(); }});
    commands_.push_back({TRANS("Restart analysis service"), "Do", "python service restart respawn stale watchdog",
                         [this]() {
                             host_.restartPythonService();
                             host_.showExplainer(TRANS("Analysis service"), TRANS("Restart"),
                                                 TRANS("Service restart requested - status shows in the sidebar."));
                         }});
    commands_.push_back({TRANS("Diagnose analysis service"), "Do",
                         "python service diagnose doctor health version routes log troubleshoot",
                         [this]() { host_.diagnoseServiceStatus(); }});
    commands_.push_back({TRANS("Delete this session and its derived data"), "Do",
                         "erase privacy gdpr remove wipe delete session stems", [this]() {
                             juce::AlertWindow::showOkCancelBox(
                                 juce::AlertWindow::WarningIcon, "Delete Session?",
                                 "This permanently deletes the active session, its analysis history, and its "
                                 "separated stems on disk. This cannot be undone.",
                                 "Delete", "Cancel", &parent_, juce::ModalCallbackFunction::create([this](int result) {
                                     if (result != 1) return;
                                     host_.deleteActiveSessionAndDerivedData([this](bool ok, juce::String error) {
                                         host_.showExplainer(TRANS("Delete session"), TRANS("Erase"),
                                                             ok ? "Session and derived data deleted."
                                                                : "Delete failed: " + error);
                                     });
                                 }));
                         }});
    commands_.push_back({TRANS("Export this session's data"), "Do",
                         "export backup portable json save session data privacy", [this]() {
                             host_.exportActiveSessionToFile([this](bool ok, juce::String messageOrPath) {
                                 host_.showExplainer(TRANS("Export session"), TRANS("Export"),
                                                     ok ? "Session data exported to:\n" + messageOrPath
                                                        : "Export failed: " + messageOrPath);
                             });
                         }});
    // CE-COL-1: shareable session bundles and the revision report.
    commands_.push_back({TRANS("Export session bundle (.khensu)"), "Do",
                         "share bundle khensu export collaborate send comments versions", [this]() {
                             host_.runCollabCommand(ICommandPaletteHost::CollabCommand::ExportBundle);
                         }});
    commands_.push_back({TRANS("Import session bundle (.khensu)"), "Do",
                         "share bundle khensu import open collaborate receive comments versions", [this]() {
                             host_.runCollabCommand(ICommandPaletteHost::CollabCommand::ImportBundle);
                         }});
    commands_.push_back({TRANS("Compare revisions (what changed / fixed / worse)"), "Do",
                         "revision compare diff before after fixed worse report version", [this]() {
                             host_.runCollabCommand(ICommandPaletteHost::CollabCommand::CompareRevisions);
                         }});
    commands_.push_back({TRANS("Back up Khensu database"), "Do",
                         "backup export database restore snapshot save archive", [this]() {
                             host_.backupDatabaseToFile([this](bool ok, juce::String messageOrPath) {
                                 host_.showExplainer(TRANS("Back up database"), TRANS("Backup"),
                                                     ok ? "Database backed up to:\n" + messageOrPath
                                                        : "Backup failed: " + messageOrPath);
                             });
                         }});
    commands_.push_back({TRANS("Delete ALL Khensu data"), "Do",
                         "erase privacy gdpr remove wipe delete everything factory reset", [this]() {
                             juce::AlertWindow::showOkCancelBox(
                                 juce::AlertWindow::WarningIcon, "Delete ALL Khensu Data?",
                                 "This permanently deletes every session, analysis, and feedback record across "
                                 "the entire app. This cannot be undone.",
                                 "Delete Everything", "Cancel", &parent_,
                                 juce::ModalCallbackFunction::create([this](int result) {
                                     if (result != 1) return;
                                     host_.deleteAllKhensuData([this](bool ok, juce::String error) {
                                         host_.showExplainer(TRANS("Delete all Khensu data"), TRANS("Erase"),
                                                             ok ? "All Khensu data deleted."
                                                                : "Delete failed: " + error);
                                     });
                                 }));
                         }});
}

} // namespace khensu
