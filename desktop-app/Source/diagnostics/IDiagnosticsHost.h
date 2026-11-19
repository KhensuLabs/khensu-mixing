// desktop-app/Source/diagnostics/IDiagnosticsHost.h
#pragma once
#include <JuceHeader.h>
#include <functional>
#include <vector>
#include "../services/IShell.h" // ScreenType

namespace khensu {

struct AnalysisResult;

/// What the whole-app diagnostics (`--a11y-audit`, `--ui-bench`,
/// `--screenshot`, `--screenshot-audio`; diagnostics/AppDiagnostics.cpp) need
/// from the window (ENG-1 step 8). The shell implements it; the diagnostics
/// never see AppController, the services or the screens' concrete types.
/// Message thread only.
class IDiagnosticsHost {
  public:
    virtual ~IDiagnosticsHost() = default;

    struct Screen {
        const char* name;     ///< "Mix / Master" - report rows
        const char* shotName; ///< "05-mix-master" - PNG file stem
        ScreenType type;
        juce::Component* component; ///< built (null only if construction failed)
        /// A drawer over the current screen (Help, CUT-4): laid out against
        /// the content area's right edge at full height, not over all of it.
        bool drawer = false;
    };
    /// Every screen, in sidebar order.
    virtual std::vector<Screen> diagnosticScreens() = 0;

    virtual juce::Component& window() = 0;      ///< the whole app component
    virtual juce::Component& contentArea() = 0; ///< where screens are laid out
    virtual juce::Component& sidebarComponent() = 0;
    virtual juce::Component& transportComponent() = 0;

    virtual void showScreen(ScreenType screen) = 0;
    /// Ends any screen fade-in so a capture isn't taken at alpha 0.
    virtual void finishScreenTransition() = 0;

    /// Publishes `result` as the session's finished analysis (health strip +
    /// every analysis listener) - the bench's fixture and update pass.
    virtual void publishAnalysisForDiagnostics(const AnalysisResult& result) = 0;
    /// The session-switch reset, without switching (bench memory check).
    virtual void resetSessionScopedState() = 0;

    /// True once the Python service has answered a health check.
    virtual bool serviceSeenHealthy() const = 0;
    /// Starts a throwaway session on `audio`; false if it could not start.
    virtual bool startDiagnosticSession(const juce::File& audio) = 0;
    /// Deletes the active session and its derived data, then calls `done`.
    virtual void deleteDiagnosticSession(std::function<void()> done) = 0;

    /// The results gate and the stem separation as the screenshot run logs them.
    struct GateSnapshot {
        bool pending = false;       ///< findings withheld
        bool overlayCovers = false; ///< the pending overlay covers the content area
        int gate = 0;               ///< AnalysisService::ResultsGate as int (log only)
        bool open = false;
        bool ended = false; ///< failed or cancelled
        int pendingScans = 0;
        bool inFlight = false;
        bool hasResult = false;
        bool deep = false; ///< the result includes the Python analysis
        bool stemsRunning = false;
        bool stemsAvailable = false;
        juce::String stemMessage;
        juce::String currentRender;     ///< the accepted version ("" = the original)
        bool verifying = false;         ///< the fix check is re-analysing it (FEAT-1)
        juce::String verification;      ///< the check's summary once it has run
    };
    virtual GateSnapshot gateSnapshot() const = 0;
    /// Fixes the top open issue through the issue queue (CON-1 / WF-1), as the
    /// F key does; `done` gets the render result.
    virtual void fixTopIssueForDiagnostics(std::function<void(bool ok, juce::String message)> done) = 0;
    /// The Dashboard expanded on its "All" tab, laid out, and its group cards
    /// (CON-2..8) by id - each is captured on its own (21b-card-<id>.png).
    virtual std::vector<std::pair<juce::String, juce::Component*>> dashboardCardsForDiagnostics() = 0;
    /// Shows the Export screen on step `step` (its step strip's index) and
    /// returns how many steps it has (22c-export-<n>.png per step).
    virtual int showExportStepForDiagnostics(int step) = 0;
    /// Drops `audio` on the Intake screen, as a user would (UX-12's after-drop state).
    virtual void dropIntoIntakeForDiagnostics(const juce::File& audio) = 0;
    /// Intake's Create Session is enabled (its first file is analysed).
    virtual bool intakeReadyForDiagnostics() const = 0;
};

} // namespace khensu
