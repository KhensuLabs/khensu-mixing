// desktop-app/Source/ui/shared/CommandPaletteController.h
#pragma once
#include <JuceHeader.h>
#include "CommandPaletteOverlay.h"
#include "NlCommandOverlay.h"
#include <functional>
#include <utility>
#include <vector>

namespace khensu {

/// Host interface for CommandPaletteController. Keeps the controller decoupled
/// from AppController's full surface - any component that can provide the
/// navigation catalog and the common palette actions can host a command
/// palette. Extracted as part of the CQ-08 coordinator split.
class ICommandPaletteHost {
  public:
    virtual ~ICommandPaletteHost() = default;

    /// Navigation entries in visual order: label + action. The controller wraps
    /// each label with "Go to " and the "Go" category.
    virtual std::vector<std::pair<juce::String, std::function<void()>>> navigationCommands() = 0;

    virtual bool canUndo() const noexcept = 0;
    virtual void undo() = 0;
    virtual bool canRedo() const noexcept = 0;
    virtual void redo() = 0;

    virtual void focusNextIssue() = 0;

    virtual void toggleReportMode() = 0;

    virtual bool hasAnalysis() const noexcept = 0;
    virtual void saveNamedSnapshot(const juce::String& label, std::function<void(bool)> onSaved = nullptr) = 0;

    virtual void cycleTheme() = 0;
    /// SHIP-4: "Check for updates" (default no-op for test hosts).
    virtual void checkForAppUpdates() {}

    virtual void showExplainer(const juce::String& title, const juce::String& category,
                               const juce::String& description, const juce::String& systemBrainFeatureId = {}) = 0;
    virtual void showReferenceTrackRitual() = 0;
    /// Goal -> target -> suggested chain setup (default no-op for test hosts).
    virtual void showOnboardingWizard() {}

    virtual void copyServiceLogsToClipboard() = 0;
    virtual void diagnoseServiceStatus() = 0;
    virtual void restartPythonService() = 0;

    virtual void deleteActiveSessionAndDerivedData(std::function<void(bool, juce::String)> onDone) = 0;
    virtual void exportActiveSessionToFile(std::function<void(bool, juce::String)> onDone) = 0;
    virtual void backupDatabaseToFile(std::function<void(bool, juce::String)> onDone) = 0;
    virtual void deleteAllKhensuData(std::function<void(bool, juce::String)> onDone) = 0;

    /// CE-COL-1: .khensu session bundles and the revision report (default
    /// no-op for test hosts).
    enum class CollabCommand { ExportBundle, ImportBundle, CompareRevisions };
    virtual void runCollabCommand(CollabCommand) {}

    /// UX-11: the core verbs, shared by the palette and the keyboard
    /// shortcuts (default no-op for test hosts).
    enum class CoreVerb { OpenFile, Analyse, FixTopIssue, FixAllSafe, ToggleAb, Export, Settings };
    virtual void runCoreVerb(CoreVerb) {}
    /// ENH-1: turns watching for new bounces on / off - the session's file
    /// itself, or (`folder`) newer versions beside it (default no-op).
    virtual void toggleBounceWatch(bool /*folder*/) {}
    /// UX-5: the opt-in session-health strip (default no-op for test hosts).
    virtual void toggleSessionHealthStrip() {}

    /// STU-7: things the palette finds besides its commands, and where
    /// selecting one goes. Defaults (test hosts): none.
    struct SearchTarget {
        juce::String title;    ///< the row's text
        juce::String keywords; ///< extra search words
        std::string id;        ///< card id / finding key / genre id
    };
    /// Dashboard cards; selecting one opens Analysis on it (its group tab
    /// selected, scrolled to and flashed).
    virtual std::vector<SearchTarget> searchableCards() { return {}; }
    virtual void revealCard(const std::string& /*cardId*/) {}
    /// The session's open findings; selecting one opens it in Issues.
    virtual std::vector<SearchTarget> searchableFindings() { return {}; }
    virtual void openFinding(const std::string& /*key*/) {}
    /// Every genre; selecting one shows its profile in the Reference
    /// Library - never changes the session's target.
    virtual std::vector<SearchTarget> searchableGenres() { return {}; }
    virtual void showGenre(const std::string& /*genreId*/) {}

    /// CE-AG-4: the controller seam free-text mix instructions run against
    /// (/command/parse -> preview -> confirm -> /command/execute). nullptr
    /// (the default, e.g. test hosts) disables the palette's "Do it" row.
    virtual IAppController* commandAppController() { return nullptr; }
};

/// Owns the lifecycle of the command-palette overlay and builds the command
/// list from an ICommandPaletteHost. Extracted from AppController (CQ-08) so
/// the palette concern (overlay + command catalog) lives in its own unit
/// instead of a ~2.7k-line coordinator.
class CommandPaletteController {
  public:
    CommandPaletteController(ICommandPaletteHost& host, juce::Component& parent);

    /// Show the palette if hidden; hide it if already visible.
    void toggle();

    /// Explicitly show the palette.
    void show();

    /// Explicitly hide the palette.
    void hide();

    bool isVisible() const noexcept { return activeCommandPalette_ != nullptr; }

    /// Update the overlay bounds when the parent resizes.
    void resized(const juce::Rectangle<int>& bounds);

    /// Handle Ctrl/Cmd+K toggle. Returns true if consumed.
    bool handleKeyPress(const juce::KeyPress& key);

  private:
    void buildCommands();

    ICommandPaletteHost& host_;
    juce::Component& parent_;
    std::vector<CommandPaletteOverlay::Command> commands_;
    std::unique_ptr<CommandPaletteOverlay> activeCommandPalette_;
    /// CE-AG-4: the open free-text instruction preview, if any.
    void showNlCommand(IAppController& app, const juce::String& text);
    std::unique_ptr<NlCommandOverlay> activeNlCommand_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CommandPaletteController)
};

} // namespace khensu
