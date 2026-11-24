#pragma once
// The app shell (ENG-1): the window, the screens, overlays, keyboard
// shortcuts and the command palette, plus the services it owns and wires
// together. Panels depend on the narrowest interface they need
// (IAppController / IBackgroundWork / services/I*.h), not on this header.
#include <JuceHeader.h>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>
#include "IAppController.h"
#include "diagnostics/IDiagnosticsHost.h"
#include "session/AnalysisService.h"
#include "session/BackgroundRunner.h"
#include "session/CloudChatService.h"
#include "session/PlaybackService.h"
#include "session/RenderService.h"
#include "session/IssueService.h"
#include "session/ServiceLifecycle.h"
#include "session/SessionService.h"
#include "session/StemService.h"
#include "session/VocalEditService.h"
#include "ui/shared/AnalysisPendingOverlay.h"
#include "ui/shared/AppNoticeBar.h"
#include "ui/shared/CommandPaletteController.h"
#include "ui/shared/DataLifecycleController.h"
#include "ui/shared/IntakeProgressHud.h"
#include "ui/shared/KhensuLookAndFeel.h"
#include "ui/shared/KhensuTheme.h" // ThemeListener
#include "ui/shared/SessionStateStrip.h"
#include "ui/shared/AbControlStrip.h"
#include "ui/shared/PlaybackTransportBar.h"
#include "ui/shared/RenderQueueStrip.h"
#include "ui/shared/SessionHealthStrip.h"
#include "ui/shared/SidebarComponent.h"
#include "ui/shared/CoachMarkCard.h"

namespace khensu {

class IntakeScreen;
class DashboardScreen;
class VocalWorkspaceScreen;
class MixMasterScreen;
class SystemBrainPanel;
class ExportScreen;
class CopilotScreen;
class IssuesScreen;
class ReferenceLibraryScreen;
class SettingsScreen;
class AiDetectionScreen;
class Midi2DeviceManager;
class MacroControlRouter;
class ExplainerOverlay;
class OnboardingWizard;
struct OnboardingPlan;
class ChainReasoningOverlay;
class MetricDiffOverlay;
class AbxTrialOverlay;
class ReferenceTrackRitualOverlay;
class FeedbackManager;

// THREAD_CONTEXT(khensu::ThreadContext::UIThread)

class AppController final : public juce::Component,
                            public IAppController,
                            public ICommandPaletteHost,
                            public ISessions,
                            public IShell,
                            private IDiagnosticsHost,
                            private juce::Timer {
  public:
    AppController();
    ~AppController() override;

    // ---- IAppController hub (ENG-1): the object that implements each slice.
    IPlayback& playback() noexcept override { return playback_; }
    IRenders& renders() noexcept override { return renders_; }
    IStems& stems() noexcept override { return stems_; }
    IVocalEdit& vocalEdit() noexcept override { return vocalEdit_; }
    IAnalysisHub& analysis() noexcept override { return analysis_; }
    ISessions& sessions() noexcept override { return *this; }
    IServiceLifecycle& lifecycle() noexcept override { return lifecycle_; }
    ICloudChat& cloudChat() noexcept override { return cloudChat_; }
    IShell& shell() noexcept override { return *this; }
    IIssues& issues() noexcept override { return issues_; }

    void paint(juce::Graphics& g) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress& key) override;

    // ---- IBackgroundWork (BackgroundRunner, drained first in ~AppController).
    void runInBackground(std::function<void()> job) override { runner_.run(std::move(job)); }
    void runLongInBackground(std::function<void()> job) override { runner_.runLong(std::move(job)); }
    bool isShuttingDown() const noexcept override { return runner_.isShuttingDown(); }
    uint64_t sessionEpoch() const noexcept override { return runner_.sessionEpoch(); }

    // ---- IAppController's direct reads and render entry points; each
    // forwards to the service that owns it. Documented on the interfaces.
    const GenreDatabase& genreDatabase() const noexcept override { return analysis_.genreDatabase(); }
    bool hasAnalysis() const noexcept override { return analysis_.hasAnalysis(); }
    const AnalysisResult& currentAnalysis() const noexcept override { return analysis_.currentAnalysis(); }
    juce::File getStemFile(const juce::String& stemName) const override { return stems_.getStemFile(stemName); }
    juce::String currentVocalStem() const override { return stems_.currentVocalStem(); }
    juce::String currentAuditionPath() const override { return playback_.currentAuditionPath(); }
    void setAbProcessedPath(const juce::String& path) noexcept override { playback_.setAbProcessedPath(path); }
    RenderJobManager& renderJobs() noexcept override { return renders_.renderJobs(); }
    juce::String currentRenderPath() const override { return renders_.currentRenderPath(); }
    void registerAppliedRender(const juce::String& renderedPath) override { renders_.registerAppliedRender(renderedPath); }
    void registerAppliedRender(const juce::String& renderedPath, const juce::String& expectedSessionId) override
    {
        renders_.registerAppliedRender(renderedPath, expectedSessionId);
    }
    void registerDerivedRender(const juce::String& renderedPath, const juce::String& expectedSessionId) override
    {
        renders_.registerDerivedRender(renderedPath, expectedSessionId);
    }
    void saveNamedSnapshot(const juce::String& label, std::function<void(bool)> onSaved = nullptr) override
    {
        renders_.saveNamedSnapshot(label, std::move(onSaved));
    }

    // ---- ISessions: the store lives in SessionService; starting, loading and
    // deleting a session is orchestrated here (AppController_Session.cpp).
    bool hasActiveSession() const noexcept override { return sessions_.hasSession(); }
    const Session* getActiveSession() const noexcept override { return sessions_.active(); }
    juce::String activeSourcePath() const override { return sessions_.activeSourcePath(); }
    int startSessionPerFile(const std::vector<AudioFileRef>& files, const CreativeTarget& target) override;
    void notifyProjectsChanged() override;
    void addProjectsListener(void* key, std::function<void()> cb) override { projectsListeners_[key] = std::move(cb); }
    void removeProjectsListener(void* key) override { projectsListeners_.erase(key); }
    bool startSession(const juce::String& name, const std::vector<AudioFileRef>& files,
                      const CreativeTarget& target) override;
    bool loadSessionById(const std::string& id) override;
    std::vector<Session> listRecentSessions(int limit = 10) override;
    std::vector<SessionManager::AnalysisSummary> listRecentAnalysisSummaries(int limit = 10) override;
    SessionManager& sessionManager() noexcept override { return sessions_.manager(); }
    std::shared_ptr<SessionManager> sessionManagerPtr() const noexcept override { return sessions_.sharedManager(); }
    bool updateActiveSessionTarget(const CreativeTarget& target) override;
    bool renameSession(const std::string& id, const juce::String& name) override;
    bool setSessionNote(const std::string& id, const juce::String& note) override;
    void enqueueProvenanceEvent(const juce::String& sessionId, const juce::String& eventType,
                                const juce::String& summary, const juce::String& recommendationId = {},
                                const juce::var& payload = {}) override;
    void addSessionScoped(SessionScoped* scoped) override { sessions_.addScoped(scoped); }
    void removeSessionScoped(SessionScoped* scoped) override { sessions_.removeScoped(scoped); }
    // Data portability and erasure (audit DATA-02); the irreversible ones
    // need the caller to confirm with the user first.
    void exportActiveSessionToFile(std::function<void(bool ok, juce::String messageOrPath)> onDone) override;
    void backupDatabaseToFile(std::function<void(bool ok, juce::String messageOrPath)> onDone) override;
    void runScheduledBackupIfDue() override;
    void deleteActiveSessionAndDerivedData(std::function<void(bool ok, juce::String error)> onDone) override;
    void deleteAllKhensuData(std::function<void(bool ok, juce::String error)> onDone) override;

    // ---- IShell: navigation, overlays and app-wide UI state.
    void showScreen(ScreenType screen) override;
    ReportMode getReportMode() const noexcept override { return reportMode_; }
    void setReportMode(ReportMode mode) override;
    void addReportModeListener(void* key, std::function<void(ReportMode)> cb) override;
    void removeReportModeListener(void* key) override;
    void showExplainer(const juce::String& title, const juce::String& category, const juce::String& description,
                       const juce::String& systemBrainFeatureId = {}) override;
    void showMetricExplanation(int gauge) override;
    void showChainReasoning(const Recommendation& rec) override;
    void showMixMasterCard(int tabIdx) override;
    void showMetricDiff(const juce::String& title, std::vector<MetricDiffTable::Row> rows, const juce::String& labelA,
                        const juce::String& labelB) override;
    void showAbxTrial(const juce::StringArray& blindReferencePaths = {}) override;
    void showReferenceTrackRitual() override;
    void showOnboardingWizard() override;
    void showMacroMappingWindow() override;
    void selectSystemBrainFeature(const std::string& id) override;
    void focusNextIssue() override;
    void loopRegion(double startSec, double endSec) override;
    void stopLoopRegion() override;
    bool isLoopingRegion(double startSec, double endSec) const noexcept override;
    void cycleTheme() override;
    void checkForAppUpdates() override;
    void showServiceError(const juce::String& message) override;
    void showGuardrailOverridePrompt(const GuardrailRejection& rejection) override;
    Midi2DeviceManager& midi2DeviceManager() noexcept override { return *midi2DeviceManager_; }
    MacroControlRouter& macroRouter() noexcept override { return *macroRouter_; }

    // ---- ICommandPaletteHost (CQ-08): the palette's app-specific actions.
    std::vector<std::pair<juce::String, std::function<void()>>> navigationCommands() override;
    void toggleReportMode() override;
    void runCollabCommand(CollabCommand command) override; // AppController_Collab.cpp
    void runCoreVerb(CoreVerb verb) override;
    void browseForAudioFile() override { runCoreVerb(CoreVerb::OpenFile); }
    void fixTopIssue() override { runCoreVerb(CoreVerb::FixTopIssue); }
    void showIssue(const std::string& key) override;
    void toggleSessionHealthStrip() override;
    static bool textInputHasFocus();
    IAppController* commandAppController() override { return this; }
    // WF-13: one undo. A focused editor with its own history (the pitch
    // editor) takes Ctrl+Z first - JUCE asks the focused component before
    // this. Otherwise the Vocal screen undoes its strategy / macro edits and
    // every other screen steps back through the render history.
    void undo() override;
    void redo() override;
    bool canUndo() const noexcept override;
    bool canRedo() const noexcept override;
    void restartPythonService() override { lifecycle_.restartPythonService(); }
    void copyServiceLogsToClipboard() override { lifecycle_.copyServiceLogsToClipboard(); }
    void diagnoseServiceStatus() override { lifecycle_.diagnoseServiceStatus(); }

  private:
    // Coalesced autosave of the vocal strategy / macros / workspace: one
    // worker writes the newest payload (AppController_Session.cpp). Declared
    // first, as it always was, so it outlives every member.
    struct AutosaveQueue {
        struct Payload {
            std::string sessionId, strategy, macrosJson, workspaceJson;
        };
        std::mutex mutex;
        std::optional<Payload> pending;
        bool workerRunning = false;
    };
    std::shared_ptr<AutosaveQueue> autosaveQueue_ = std::make_shared<AutosaveQueue>();
    void autosaveState();
    void restoreAutosavedState();
    /// Loads the open session's intake reference (target.reference_track_paths)
    /// as the comparison reference when nothing restored one (P0-4).
    void loadTargetReference();

    KhensuLookAndFeel lookAndFeel;

    // ---- The services (ENG-1). Every background job they start runs on
    // runner_, drained first in ~AppController; their UI hooks are wired in
    // the constructor and fire on the message thread. A service is declared
    // before any service holding a reference to it, so it is destroyed after.
    SessionService sessions_; // store, open session, session-reset registry
    // Python service process/health/restart, outbox, Copilot plugin link;
    // its polls use runner_'s poll / restart pools.
    ServiceLifecycle lifecycle_{*this, sessions_,
                                [this](std::function<void()> job) { runner_.runPoll(std::move(job)); },
                                [this](std::function<void()> job) { return runner_.runRestart(std::move(job)); }};
    CloudChatService cloudChat_;          // disclosure + consent
    StemService stems_{*this, sessions_}; // separation + vocal-stem analysis
    // The analysis, the results gate and the reference data; the screens
    // deregister from its listeners in ~AppController's body.
    AnalysisService analysis_{*this, sessions_};
    std::shared_ptr<FeedbackManager> feedbackMgr;
    std::unique_ptr<Midi2DeviceManager> midi2DeviceManager_;
    std::unique_ptr<MacroControlRouter> macroRouter_; // after midi2DeviceManager_: destroyed first
    std::unique_ptr<juce::DocumentWindow> macroMappingWindow_; // after macroRouter_: destroyed first
    // UI-10: the first view is calm - the verdict, the track and the one
    // thing to fix. Engineer Detail (the per-stage strips, keyboard legend,
    // completeness chip, the full card grid) is one click away.
    ReportMode reportMode_ = ReportMode::ClientSummary;
    std::map<void*, std::function<void(ReportMode)>> reportModeListeners_;
    /// FEAT-6. Declared before the screens: their panels deregister on destruction.
    std::map<void*, std::function<void()>> projectsListeners_; ///< FEAT-6

    // A loaded session whose source file is gone: offer to relocate it.
    std::unique_ptr<juce::FileChooser> missingFileChooser_;
    void handleMissingSessionFile();
    /// FEAT-7: imports the plugin's captures as sessions and offers to open each.
    void scanPluginCaptures();
    std::shared_ptr<std::atomic<bool>> captureScanRunning_ = std::make_shared<std::atomic<bool>>(false);

    SidebarComponent sidebar_;
    juce::Component contentContainer;
    std::unique_ptr<IntakeScreen> intakeScreen;
    std::unique_ptr<DashboardScreen> dashboardScreen;
    std::unique_ptr<VocalWorkspaceScreen> vocalWorkspaceScreen;
    std::unique_ptr<MixMasterScreen> mixMasterScreen;
    std::unique_ptr<SystemBrainPanel> systemBrainPanel;
    std::unique_ptr<ExportScreen> exportScreen;
    std::unique_ptr<CopilotScreen> copilotScreen;
    std::unique_ptr<IssuesScreen> issuesScreen;
    std::unique_ptr<ReferenceLibraryScreen> referenceLibraryScreen;
    std::unique_ptr<SettingsScreen> settingsScreen;
    std::unique_ptr<AiDetectionScreen> aiDetectionScreen;
    /// PERF-5: builds a lazy screen on first use (adds it to the content
    /// container, lays it out, names its controls) and returns it.
    template <typename ScreenT>
    juce::Component* ensureLazyScreen(std::unique_ptr<ScreenT>& slot);
  public:
    /// The lazy screen for `screen`, built if needed (diagnostics visit every
    /// screen); nullptr for the screens built at launch.
    juce::Component* ensureScreenBuilt(ScreenType screen);
  private:
    juce::ComponentAnimator screenAnimator_;

    ScreenType currentScreen = ScreenType::Intake;
    // CUT-4: Help (System Brain) is a drawer over the current screen, not a
    // screen of its own; currentScreen stays the screen underneath it.
    bool helpDrawerOpen_ = false;
    // FEAT-8: the looped region (end <= start = none), wrapped in the tick.
    double loopStart_ = 0.0, loopEnd_ = 0.0;
    // UX-15: the first session's three steps (shown once, ever).
    std::unique_ptr<CoachMarkCard> coachCard_;
    bool suppressCoach_ = false; ///< diagnostics sessions never show it
    void maybeShowCoach();
    void layoutCoach();
    void openHelpDrawer();
    void closeHelpDrawer();
    juce::Rectangle<int> helpDrawerBounds() const;
    std::unique_ptr<ExplainerOverlay> activeOverlay;
    std::unique_ptr<OnboardingWizard> onboardingWizard_;
    void finishOnboarding(const OnboardingPlan& plan, bool pushToPlugin);
    std::unique_ptr<ChainReasoningOverlay> activeReasoningOverlay_;
    std::unique_ptr<MetricDiffOverlay> activeMetricDiffOverlay_;
    std::unique_ptr<AbxTrialOverlay> activeAbxOverlay_;
    CommandPaletteController commandPaletteController_;
    std::optional<DataLifecycleController> dataLifecycleController_;
    std::unique_ptr<ReferenceTrackRitualOverlay> activeReferenceRitual_;

    // Renders, the render history and the job queue. playback_ is declared
    // further down: RenderService only stores the reference while constructing.
    RenderService renders_{*this, sessions_, playback_, stems_, analysis_};
    RenderQueueStrip renderQueueStrip_{renders_.renderJobs()};
    // The one issue queue (CON-1); registers with analysis_ and renders_ in
    // the constructor body (attach()), deregisters in its destructor.
    IssueService issues_{analysis_, renders_, sessions_, lifecycle_};

    SessionHealthStrip sessionHealthStrip_{*this}; // listening-fatigue coach
    IntakeProgressHud intakeProgressHud_;          // analysis progress on every screen (spec 9.1)

    juce::Label serviceErrorBanner_; // top-of-window toast
    juce::String lastServiceError_;
    juce::int64 lastServiceErrorMs_ = 0;
    AppNoticeBar noticeBar_; // crash report saved, update out (AppController_Notices.cpp)
    AnalysisPendingOverlay resultsOverlay_; // the results gate (analysis_.resultsGate())
    bool updateCheckStarted_ = false;
    void postCrashNoticeIfAny();
    /// `manual`: the user asked - skip the throttle and report every outcome,
    /// not only a newer release.
    void startUpdateCheck(bool manual = false);
    bool manualUpdateCheckInFlight_ = false;
    int updateDownloadPolls_ = 0; // re-checks while the service downloads an update
    // The results gate's on-screen half; the state machine lives in
    // analysis_ (ENG-1 stage C5), whose hooks drive these.
    bool stemsAutoStartPending_ = false; // new session: separate stems once results are released
    /// analysis_.onGateChanged: overlay mode, health strip, status, stems.
    void applyResultsGate(AnalysisService::ResultsGate gate);
    void updateResultsOverlay();
    bool screenShowsResults(ScreenType s) const noexcept;
    // A new "show_reasoning" event from the cross-process event poll (lifecycle_).
    void handleShowReasoningEvent(const juce::String& recId);
    // The gave-up notice (banner + restart prompt) for lifecycle_.
    void showServiceGaveUpNotice(bool portTaken);
    /// UX-7: one app-wide notice while the analysis engine is starting or
    /// down, withdrawn once it runs (AppController_Notices.cpp).
    void updateServiceBanner(PythonStatus status, int restartAttempt);
    /// UX-5: session-only chrome (the metric rollup, the opt-in session-health
    /// strip) shows only with a session.
    void updateSessionChrome();
    std::optional<bool> sessionHealthStripEnabled_; ///< settings.json ui.session_health_strip, read once
    int serviceBannerGeneration_ = 0;

    /// Demucs separation for a new session once its results are released, so
    /// stems are usually ready before the Vocal Workspace is opened.
    void autoStartStemSeparation();
    /// Clears everything that belongs to the previous session (renders, A/B,
    /// vocal edits, stems); called before a session starts, loads or is deleted.
    void resetSessionScopedState() override;

    VocalEditService vocalEdit_; // strategy, macros, undo; onChanged -> autosaveState()

    void updateSessionStatusLabel();

    PlaybackService playback_{*this, sessions_, stems_, analysis_};
    PlaybackTransportBar playbackBar;
    /// VIZ-P1: the top strip - session state only (version, open issues, the
    /// last fix check, target, analysis); measurements live in the tiles and
    /// the transport.
    SessionStateStrip sessionStateStrip_;
    SessionStateStrip::State::Analysis analysisState_ = SessionStateStrip::State::Analysis::None;
    void refreshSessionStateStrip();
    /// VIZ-U2: on first run, the demo song opens as a sample session (once).
    void openDemoSessionOnFirstRun();
    /// UI-6: the first-class A | B control in the top strip (session-only).
    AbControlStrip abControlStrip_{*this};
    static constexpr int kTopStripH = 40;
    juce::TextButton reportModeButton_{TRANS("Engineer Detail")};
    juce::TooltipWindow tooltipWindow_{this}; // makes every setTooltip() in the app actually render
    // Worker pools, session epoch, shutdown flag: drained first thing in
    // ~AppController, before any member a job could touch goes away.
    BackgroundRunner runner_;

    // Hourly check for the daily backup (DATA-02); our own Timer is the 50 ms
    // UI tick. After runner_, so it stops before the pools drain.
    struct ScheduledBackupTimer final : juce::Timer {
        explicit ScheduledBackupTimer(AppController& o) : owner(o) {}
        void timerCallback() override { owner.dataLifecycleController_->runScheduledBackupIfDue(); }
        AppController& owner;
    } scheduledBackupTimer_{*this};

    int timerTicks = 0;

    void invalidateBackgroundReads() noexcept;

    // ---- Overlays, reasoning traces and keyboard help (message thread).
    void applyReportModeButtonStyle();
    void hideExplainer();
    void showSystemBrainFeature(const juce::String& featureId);
    void hideChainReasoning();
    /// The reasoning overlay for any metric verdict, from plain-language fields.
    void showMetricReasoning(const juce::String& detected, const juce::String& cause, const juce::String& fix,
                             const juce::String& tradeOff, const juce::String& verify);
    /// Reasoning for a core metric (0 loudness, 1 crest, 2 mids, 3 correlation, 4 clips).
    void showHealthMetricReasoning(int metricIndex);
    void hideMetricDiff();
    void hideAbxTrial();
    void hideReferenceTrackRitual();
    /// POST /copilot/override_guardrail in the background; reports the outcome.
    void requestGuardrailOverride(const GuardrailRejection& rejection, const juce::String& expertNote);
    /// F6 / Shift+F6: move keyboard focus between sidebar, screen, transport.
    void cycleFocusRegion(bool backwards);
    /// F1: the keyboard shortcut reference.
    void showKeyboardHelp();

    // ---- IDiagnosticsHost (ENG-1 step 8): what --ui-bench, --a11y-audit and
    // --screenshot need from the window; diagnostics/AppDiagnostics.cpp. ----
    std::vector<IDiagnosticsHost::Screen> diagnosticScreens() override;
    juce::Component& window() override { return *this; }
    juce::Component& contentArea() override { return contentContainer; }
    juce::Component& sidebarComponent() override { return sidebar_; }
    juce::Component& transportComponent() override { return playbackBar; }
    void finishScreenTransition() override { screenAnimator_.cancelAllAnimations(true); }
    void publishAnalysisForDiagnostics(const AnalysisResult& result) override;
    bool serviceSeenHealthy() const override { return updateCheckStarted_; }
    int serviceStatusForDiagnostics() const override { return (int)lifecycle_.getPythonServiceStatus(); }
    bool startDiagnosticSession(const juce::File& audio) override;
    void deleteDiagnosticSession(std::function<void()> done) override;
    GateSnapshot gateSnapshot() const override;
    void fixTopIssueForDiagnostics(std::function<void(bool, juce::String)> done) override;
    std::vector<std::pair<juce::String, juce::Component*>> dashboardCardsForDiagnostics() override;
    int showExportStepForDiagnostics(int step) override;
    int showMixMasterViewForDiagnostics(int view) override;
    void playForDiagnostics(double fromSec, bool play) override;
    void dropIntoIntakeForDiagnostics(const juce::File& audio) override;
    bool intakeReadyForDiagnostics() const override;

    void timerCallback() override;
    /// Re-applies every cached palette colour. Called at construction and on
    /// each theme change.
    void applyThemeColours();
    // Construction steps (REF-8), called once from the constructor in this order.
    void wireServiceCallbacks();
    void writeServiceToken();
    void openStores();
    void loadBundledReferenceData();
    void buildScreens();
    void buildChrome();

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AppController)
};

} // namespace khensu
