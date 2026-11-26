#include "AppController.h"
#include "ui/shared/ReducedMotion.h"
#include "ui/shared/AppFileDrop.h"
#include "ui/shared/DecisionNotices.h"
#include <khensu/dsp/DemoSong.h>
#include "BundledResources.h"
#include "session/KeyedListeners.h"
#include "ui/shared/AbxTrialOverlay.h"
#include "ui/shared/ChainReasoningOverlay.h"
#include "ui/shared/ExplainerOverlay.h"
#include "ui/shared/MetricDiffOverlay.h"
#include "ui/shared/OnboardingWizard.h"
#include "ui/shared/ReferenceTrackRitualOverlay.h"
#include "diagnostics/AppDiagnostics.h"
#include "midi/MacroControlRouter.h"
#include "ui/Settings/MacroMappingPanel.h"
#include "ui/shared/AccessibilityCoverage.h"
#include "BackgroundReadKey.h"
#include "ui/shared/KhensuTheme.h"
#include "ui/shared/KhensuLookAndFeel.h"
#include "ui/shared/LiquidSmokeBackdrop.h"
#include <cmath>
#include <thread>
#include <khensu/common/AppPaths.h>
#include <khensu/common/FileAcl.h>
#include <khensu/common/SecureRandom.h>
#include <khensu/feedback/FeedbackManager.h>
#include <khensu/ipc/HttpServiceBridge.h>
#include <khensu/analysis/AudioAnalyzer.h>
#include <khensu/recommendations/RecommendationEngine.h>
#include <khensu/recommendations/MixReportBuilder.h>
#include <khensu/recommendations/ScoreWeightProfile.h>
#include <khensu/targets/UserPreferenceModel.h>
#include <juce_core/juce_core.h>
#include "ui/SessionIntake/IntakeScreen.h"
#include "ui/Dashboard/DashboardScreen.h"
#include "ui/VocalWorkspace/VocalWorkspaceScreen.h"
#include "ui/MixMasterEngine/MixMasterScreen.h"
#include "ui/SystemBrain/SystemBrainPanel.h"
#include "ui/ExportDelivery/ExportScreen.h"
#include "ui/Copilot/CopilotScreen.h"
#include "ui/Issues/IssuesScreen.h"
#include "analysis/ResultDerivedData.h" // UX2-4: the sidebar's readiness
#include "ui/shared/GenreInUse.h"        // BUG-1: the target the findings used
#include "ui/Dashboard/FindingCardMap.h" // ENH-7
#include "ui/shared/IssueQueuePanel.h"  // STU-7: a finding's title
#include "ui/ReferenceLibrary/ReferenceLibraryScreen.h"
#include "session/WorkspaceRecoveryState.h"
#include "ui/Settings/SettingsScreen.h"
#include "ui/ProvenanceSource/AiDetectionScreen.h"
#include "midi/Midi2DeviceManager.h"
#include "ui/shared/RenderPaths.h"
#include "CrashReporter.h"
#include "ui/shared/JobPoller.h"
#include <khensu/ipc/generated/ApiSchema.h>

// Service JSON field names, generated from resources/api/openapi.json:
// a renamed Python field is a compile error here, not a silent 0.
namespace apif = khensu::api::fields;

namespace khensu {

namespace {
// Screen names for a crash report's "Last screen" line (not user-facing).
const char* crashScreenName(ScreenType s) noexcept
{
    switch (s) {
    case ScreenType::Intake: return "Intake";
    case ScreenType::Dashboard: return "Dashboard";
    case ScreenType::VocalWorkspace: return "VocalWorkspace";
    case ScreenType::MixMaster: return "MixMaster";
    case ScreenType::SystemBrain: return "SystemBrain";
    case ScreenType::Export: return "Export";
    case ScreenType::Copilot: return "Copilot";
    case ScreenType::Issues: return "Issues";
    case ScreenType::ReferenceLibrary: return "ReferenceLibrary";
    case ScreenType::Settings: return "Settings";
    case ScreenType::AiDetection: return "AiDetection";
    }
    return "Unknown";
}
} // namespace

void AppController::invalidateBackgroundReads() noexcept
{
    playback_.invalidateReads(); // waveform peaks + stem re-analysis
    stems_.invalidateReads(); // vocal-stem analysis
    renders_.invalidateReads(); // in-flight delta render
    runner_.advanceEpoch();
}

// Every cached palette colour on this component, in one place (UI-01).
void AppController::applyThemeColours()
{
    serviceErrorBanner_.setColour(juce::Label::backgroundColourId, KhensuTheme::accentRed.darker(0.4f));
    serviceErrorBanner_.setColour(juce::Label::textColourId, KhensuTheme::textPrimary);
    // The report-mode button caches its colours too; it kept the old theme.
    applyReportModeButtonStyle();
}

AppController::AppController() : commandPaletteController_(*this, *this), playbackBar(*this)
{
    applyThemeColours();
    // LG-1..4: load ambient backdrop preference and start VBlank driver.
    {
        const auto s = OnlineSettings::load(juce::File(juce::String(AppPaths::dataDirectory())));
        LiquidSmokeBackdrop::setAmbientMotionEnabled(s.ambientBackdrop);
    }
    playback_.addRealTimeMeterListener(this, [this](float lp, float rp, float, float, float, float) {
        smokeAudioEnergy_ = juce::jlimit(0.0f, 1.0f, juce::jmax(lp, rp));
    });
    smokeVBlank_ = std::make_unique<juce::VBlankAttachment>(this, [this] { onSmokeVBlank(); });
    // Playback starts at the transport slider's level. Applied here, once every
    // member exists - never from PlaybackTransportBar's constructor, which runs
    // in the initializer list above (launch crash, 2026-11-06).
    playback_.setPlaybackVolume(PlaybackTransportBar::kDefaultVolume);
    wireServiceCallbacks();
    writeServiceToken();
    // Show "starting" immediately so the sidebar never flashes "stopped" while
    // the background launch thread is doing its isAvailable() probe; the
    // launch itself is fire-and-forget (token is already on disk).
    lifecycle_.launch();

    openStores();
    loadBundledReferenceData();

    analysis_.attachFeedbackManager(feedbackMgr);
    midi2DeviceManager_ = std::make_unique<Midi2DeviceManager>();
    macroRouter_ = std::make_unique<MacroControlRouter>(*this, *midi2DeviceManager_);

    // Apply our custom look and feel globally - as the app default too, so a
    // component built before it is attached (an issue row's Fix amount
    // slider) takes the theme's colours instead of JUCE's stock ones, whose
    // white value text was invisible on the Light theme.
    juce::LookAndFeel::setDefaultLookAndFeel(&lookAndFeel);
    setLookAndFeel(&lookAndFeel);

    setWantsKeyboardFocus(true);

    // Wire sidebar navigation
    sidebar_.onNavClicked = [this](int idx) { showScreen(SidebarComponent::screenTypeForVisualIndex(idx)); };
    sidebar_.onCancelAnalysis = [this]() { analysis_.cancelAnalysis(); };
    sidebar_.onGettingStarted = [this]() { showOnboardingWizard(); }; // UX-15
    addAndMakeVisible(sidebar_);
    addAndMakeVisible(contentContainer);
    updateSessionStatusLabel();

    buildScreens();
    buildChrome();

    // Show initial screen
    showScreen(ScreenType::Intake);

    startTimer(50);

    // Automatic daily backup (audit DATA-02): check once at launch, then
    // hourly - the check itself is cheap (a settings read) unless a backup
    // is actually due.
    runScheduledBackupIfDue();
    scheduledBackupTimer_.startTimer(60 * 60 * 1000);
}

// ---- Construction steps (REF-8), in the order the constructor runs them ----

// The render, stem, vocal-edit, service and analysis callbacks: everything
// those services tell the UI. Wired before the service launches so its first
// status lands.
void AppController::wireServiceCallbacks()
{
    // Post-render safety guardrail (2026-07-15 safety audit P0): warn the user
    // centrally whenever a render regresses at matched loudness, so no flow can
    // silently present a worse result as an improvement.
    renders_.renderJobs().onGuardrailAlert = [this](bool isBlock, const juce::String& details) {
        showServiceError((isBlock ? juce::String("Render REGRESSED at matched loudness - review before keeping it:\n")
                                  : juce::String("Render caution (matched loudness):\n")) +
                         details);
    };
    // A render the guardrail actually rejected/couldn't check (as opposed to
    // one that ran and regressed) never reaches the requested output path at
    // all - it's quarantined, and only an explicit expert override promotes
    // it (audit AI-01).
    renders_.renderJobs().onGuardrailRejected = [this](const GuardrailRejection& rejection) {
        showGuardrailOverridePrompt(rejection);
    };
    // The A/B loudness priming from a render's guardrail (AUDIT-038) is set
    // up by RenderService itself; a changed accepted render is persisted here.
    issues_.attach(); // CON-1: after analysis_ and renders_ exist (ENG-2)
    renders_.onAcceptedRenderChanged = [this] {
        autosaveState();
        // FEAT-1: every accepted version is re-analysed and checked against
        // the original's findings ("Did it work?"); back to the original
        // clears the check.
        analysis_.verifyRender(renders_.currentRenderPath());
    };
    // AUTO-3: an edit of an earlier version offers to re-run the fixes that
    // were stacked on it; the queue's progress and its Cancel are a notice.
    renders_.onReplayOffer = [this](const RenderService::ReplayOffer& offer) {
        noticeBar_.withdraw("rerun-offer");
        const int n = (int)offer.steps.size();
        juce::String text = n == 1 ? TRANS("You changed an earlier version. Re-run the fix that was stacked on it?")
                                   : TRANS("You changed an earlier version. Re-run the %n fixes that were stacked on it, in order?")
                                         .replace("%n", juce::String(n));
        if (offer.unknownSteps > 0)
            text << " " << TRANS("(%u could not be re-run: how they were made is not recorded.)")
                               .replace("%u", juce::String(offer.unknownSteps));
        AppNoticeBar::Notice notice;
        notice.id = "rerun-offer";
        notice.text = text;
        notice.actionLabel = n == 1 ? TRANS("Re-run 1 fix") : TRANS("Re-run %n fixes").replace("%n", juce::String(n));
        juce::Component::SafePointer<AppController> self(this);
        notice.onAction = [self, offer] {
            if (self != nullptr) self->renders_.startReplay(offer);
        };
        noticeBar_.post(std::move(notice));
    };
    renders_.onReplayProgress = [this](int done, int total, const juce::String& status, bool finished, bool ok) {
        noticeBar_.withdraw("rerun-progress");
        AppNoticeBar::Notice notice;
        notice.id = "rerun-progress";
        notice.text = status;
        if (!finished) {
            notice.text << " (" << juce::String(done) << "/" << juce::String(total) << ")";
            notice.actionLabel = TRANS("Cancel");
            juce::Component::SafePointer<AppController> self(this);
            notice.onAction = [self] {
                if (self != nullptr) self->renders_.cancelReplay();
            };
        }
        juce::ignoreUnused(ok);
        noticeBar_.post(std::move(notice));
    };
    // OVH-9: while the engine is not Running, native-renderable fixes render
    // in-process (the service stays the renderer whenever it is up).
    renders_.serviceAvailable = [this] { return lifecycle_.getPythonServiceStatus() == PythonStatus::Running; };
    // Stem separation's stage-6 progress drives the intake HUD and the
    // pipeline-progress listeners; vocal strategy/macro edits are persisted.
    stems_.onStemProgress = [this](const std::string& stage, int idx, int total) {
        intakeProgressHud_.updateStage(stage, idx, total);
        analysis_.notifyPipelineProgress(stage, idx, total);
    };
    vocalEdit_.onChanged = [this] { autosaveState(); };
    stems_.onCurrentVocalChanged = [this] { autosaveState(); }; // WF-4
    // The Python service's status, healthy edge, gave-up notice, plugin
    // meters, reasoning events and reports reach the UI through these
    // (ENG-1 stage C4). Wired before launch() so its first status lands.
    lifecycle_.onStatusChanged = [this](PythonStatus status, int restartAttempt) {
        // OVH-1: the lifecycle's status is the one answer to "is the engine
        // there" - the deep pass reads it, and a pass skipped for want of the
        // engine re-runs once it is Running.
        analysis_.setEngineStatus(status);
        sidebar_.setPythonStatus(status, restartAttempt);
        updateServiceBanner(status, restartAttempt);
    };
    lifecycle_.onServiceRunning = [this] {
        if (!updateCheckStarted_) {
            updateCheckStarted_ = true; // once per launch; the service throttles to hourly
            startUpdateCheck();
        }
    };
    lifecycle_.onServiceRecovered = [this] { analysis_.reanalyzeIfServiceRecovered(); };
    lifecycle_.onGaveUp = [this](bool portTaken) { showServiceGaveUpNotice(portTaken); };
    lifecycle_.onPluginPlayhead = [this](const ServiceLifecycle::PlayheadState& s) {
        playback_.publishMeters(s.left_peak_db, s.right_peak_db, s.momentary_lufs, s.short_term_lufs, s.correlation,
                                s.width);
    };
    lifecycle_.onShowReasoningEvent = [this](const juce::String& recId) { handleShowReasoningEvent(recId); };
    lifecycle_.onShowReport = [this](const juce::String& title, const juce::String& category,
                                     const juce::String& text) { showExplainer(title, category, text, {}); };
    // The analysis and its results gate reach the UI through these (ENG-1
    // stage C5): the overlay, sidebar status, intake HUD and health strip.
    analysis_.onGateChanged = [this](AnalysisService::ResultsGate gate) { applyResultsGate(gate); };
    analysis_.onStatus = [this](const juce::String& text, bool ok, bool inProgress) {
        sidebar_.setAnalysisStatus(text, ok, inProgress);
    };
    analysis_.onStage = [this](const std::string& stage, int idx, int total) {
        intakeProgressHud_.updateStage(stage, idx, total);
        if (analysis_.resultsGate() == AnalysisService::ResultsGate::Analyzing)
            resultsOverlay_.updateStage(stage, idx, total);
    };
    analysis_.onFailed = [this](const juce::String& message) {
        resultsOverlay_.setMode(AnalysisPendingOverlay::Mode::Failed, message);
        sidebar_.setAnalysisStatus(TRANS("Analysis failed"), false, false);
        showServiceError(TRANS("Analysis failed") + ": " + message); // UX-10: the banner, not a modal
    };
    // The follow-up scans run after the results are shown (PERF-1): the
    // sidebar says so while they do, then goes back to "Ready".
    analysis_.onFollowUpsChanged = [this](int pending, int total) {
        if (analysis_.resultsGate() != AnalysisService::ResultsGate::Open) return;
        if (pending > 0)
            sidebar_.setAnalysisStatus(TRANS("Ready - more checks running") + " (" +
                                           juce::String(total - pending) + "/" + juce::String(total) + ")",
                                       true, /*inProgress*/ true);
        else
            updateSessionStatusLabel();
    };
    analysis_.onProvisionalResult = [this](const AnalysisResult& fast) {
        if (fast.has_fast_analysis())
            resultsOverlay_.setProvisional(fast.loudness.lufs_integrated, fast.loudness.true_peak_dbtp,
                                           fast.artifacts.has_clipping);
    };
    analysis_.onFinalResult = [this] {
        intakeProgressHud_.markAnalysisFinished();
        // A new analysis is a new baseline: check the current version again.
        if (renders_.currentRenderPath().isNotEmpty()) analysis_.verifyRender(renders_.currentRenderPath());
    };
    analysis_.onVerificationStatus = [this](const juce::String& text, bool inProgress) {
        if (analysis_.resultsGate() == AnalysisService::ResultsGate::Open && hasAnalysis())
            sidebar_.setAnalysisStatus(text, true, inProgress);
    };
    analysis_.onCancelled = [this] { intakeProgressHud_.reset(); };
    analysis_.onReferenceChanged = [this] { autosaveState(); };
}

// The per-launch bearer token: written to token.txt (owner-only ACL) and
// armed as the /health identity key before any request carries it.
void AppController::writeServiceToken()
{
    juce::File tokenFile(juce::String(AppPaths::dataDirectory()) + "/token.txt");
    // 32 bytes from the OS CSPRNG (AUDIT-022): juce::Uuid is a clock-seeded
    // LCG with ~48 bits of real entropy.
    const juce::String token(SecureRandom::hex(32));
    // The token is also the key of the /health identity challenge: no
    // request carrying it leaves the app until the listener proves it can
    // read token.txt (AUDIT-020).
    lifecycle_.setAuthToken(token);
    ipc::HttpServiceBridge::setServiceIdentityRequired(true);
    if (token.isEmpty() || !tokenFile.replaceWithText(token)) {
        // A failed write leaves a stale (or missing) token -> every service
        // call would 401 with no explanation. Tell the user instead of
        // failing silently.
        juce::Logger::writeToLog("AppController: FAILED to write token.txt - service auth will fail");
        // UXR-7: the app's notice bar, not a modal - it informs, nothing to decide.
        noticeBar_.post({"service-token-write",
                         TRANS("Service authentication unavailable: could not write the local service token file "
                               "(%p). Deep analysis and copilot features will not work this run - check "
                               "permissions on the application data folder.")
                             .replace("%p", tokenFile.getFullPathName()),
                         {},
                         {},
                         {}});
    } else {
        // Best-effort (audit SEC-06): the bearer token is plaintext, so
        // another local account being able to read it would let it
        // impersonate the desktop app to the Python service.
        FileAcl::restrictToCurrentUser(tokenFile.getFullPathName().toStdString());
    }
}

// khensu.db for sessions and feedback (one file, or both in memory in an
// explicit degraded mode), then the data-lifecycle and cloud-chat
// controllers that need them.
void AppController::openStores()
{
    // AUDIT-013: a busy/locked khensu.db (another process migrating it) is
    // retried with backoff instead of silently switching to ":memory:" while
    // FeedbackManager kept the real file - that split the store, lost the
    // session and let the scheduled backup save an empty DB. If it still
    // fails, BOTH stores run in memory in an explicit degraded mode with
    // backups off, and the user is told.
    bool storageDegraded = false;
    {
        auto store = SessionService::openStore(AppPaths::databaseFile());
        storageDegraded = store.degraded;
        sessions_.attachStore(std::move(store.manager));
        if (storageDegraded) {
            // UXR-7: the app's notice bar, not a modal - it informs, nothing to decide.
            noticeBar_.post({"session-storage-degraded",
                             TRANS("Session storage unavailable: Khensu is running in a temporary mode - sessions, "
                                   "feedback and settings changes made now will not be saved, and automatic backups "
                                   "are paused.") +
                                 (store.failure.isNotEmpty() ? " (" + store.failure + ")" : juce::String()),
                             {},
                             {},
                             {}});
        }
    }

    try {
        // Unified store (ARCH-03 Phase 3): feedback now lives in khensu.db, the
        // same file SessionManager opens. FeedbackManager one-time-imports the
        // legacy feedback.db (in the same dir) on construction. In degraded
        // mode it shares the in-memory fate so the two stores never split.
        feedbackMgr = std::make_shared<FeedbackManager>(storageDegraded ? std::string(":memory:")
                                                                        : AppPaths::databaseFile());
    } catch (const std::exception& e) {
        juce::Logger::writeToLog("Feedback DB open failed, using in-memory: " + juce::String(e.what()));
        feedbackMgr = std::make_shared<FeedbackManager>(":memory:");
    }
    playback_.setFeedbackManager(feedbackMgr);
    playback_.onWaveformChanged = [this] { repaint(); };

    // Data export / backup / erasure controller (CQ-08 split). Needs sessions_.sharedManager()
    // and feedbackMgr to exist, so it is emplaced after the DB constructors above.
    dataLifecycleController_.emplace(sessions_.manager(), feedbackMgr.get(),
                                     [this](std::function<void()> job) { runInBackground(std::move(job)); },
                                     juce::File(juce::String(AppPaths::dataDirectory())));
    dataLifecycleController_->setStorageDegraded(storageDegraded);
    dataLifecycleController_->retireLegacyStoreFiles(); // PKG-4: month-old upgrade leftovers

    // Cloud-chat disclosure + consent controller (CQ-08 split). Reads online
    // settings and persists per-provider consent in khensu.db.
    cloudChat_.attach(sessions_.manager(), juce::File(juce::String(AppPaths::dataDirectory())));
}

// Genre norms and the System Brain encyclopedia from the bundled resources
// (next to the executable in dev, or the install resources dir).
// Best-effort: pickers and the panel degrade to empty.
void AppController::loadBundledReferenceData()
{
    analysis_.loadReferenceData(bundled_resources::find("resources/genre_norms/genre_targets.json"),
                                bundled_resources::find("resources/system_brain/feature_encyclopedia.json"));
}

// The eleven screens, their accessibility pass, the onboarding hand-off and
// the content container.
void AppController::buildScreens()
{
    // Instantiate Screens
    intakeScreen = std::make_unique<IntakeScreen>(*this);
    dashboardScreen = std::make_unique<DashboardScreen>(*this);
    vocalWorkspaceScreen = std::make_unique<VocalWorkspaceScreen>(*this);
    mixMasterScreen = std::make_unique<MixMasterScreen>(*this);
    systemBrainPanel = std::make_unique<SystemBrainPanel>(*this);
    exportScreen = std::make_unique<ExportScreen>(*this);
    // PERF-5: Copilot, References and AI Detection are built on first visit
    // (ensureLazyScreen) - none has work to do before the user opens it.
    issuesScreen = std::make_unique<IssuesScreen>(*this);
    settingsScreen = std::make_unique<SettingsScreen>(*this);

    // `--a11y-audit=`, `--ui-bench=`, `--screenshot=[--screenshot-audio=]`:
    // whole-app checks run 3 s after launch (diagnostics/AppDiagnostics.cpp).
    diagnostics::scheduleFromCommandLine(*this);

    // Spoken names + tooltips for every control built so far; showScreen()
    // re-runs this per screen so dynamically added rows are covered too.
    for (juce::Component* c : std::initializer_list<juce::Component*>{
             intakeScreen.get(), dashboardScreen.get(), vocalWorkspaceScreen.get(), mixMasterScreen.get(),
             exportScreen.get(), copilotScreen.get(), issuesScreen.get(), referenceLibraryScreen.get(),
             settingsScreen.get(), aiDetectionScreen.get(), &sidebar_, &playbackBar})
        if (c != nullptr) AccessibilityCoverage::apply(*c);

    // P0-4: a finished onboarding's genre is Intake's starting genre.
    if (auto profile = sessionManager().getUserPreference("onboarding_profile");
        profile.is_ok() && !profile.value().empty() && intakeScreen != nullptr) {
        const auto parsed = juce::JSON::parse(juce::String(profile.value()));
        intakeScreen->presetGenre(parsed["genre_id"].toString());
    }

    // Simple first-run guided tour leveraging System Brain (high priority)
    if (listRecentSessions(1).empty()) {
        juce::Timer::callAfterDelay(1100, [safe = juce::Component::SafePointer<AppController>(this)]() {
            if (auto* self = safe.getComponent()) {
                // VIZ-U2: the first impression is a full screen - the demo
                // song opens as a sample session (once).
                self->openDemoSessionOnFirstRun();
                // First run: goal -> target -> suggested chain, unless the
                // user already finished or skipped it once.
                auto done = self->sessionManager().getUserPreference("onboarding_profile");
                if (!done.is_ok() || done.value().empty()) self->showOnboardingWizard();
            }
        });
    }

    // Add screens to content container (hidden initially)
    contentContainer.addChildComponent(intakeScreen.get());
    contentContainer.addChildComponent(dashboardScreen.get());
    contentContainer.addChildComponent(vocalWorkspaceScreen.get());
    contentContainer.addChildComponent(mixMasterScreen.get());
    contentContainer.addChildComponent(systemBrainPanel.get());
    contentContainer.addChildComponent(exportScreen.get());
    contentContainer.addChildComponent(issuesScreen.get());
    contentContainer.addChildComponent(settingsScreen.get());
}

// The app-wide chrome around the screens: transport, health strips, intake
// HUD, report-mode button, banners, results overlay and render queue.
void AppController::buildChrome()
{
    addAndMakeVisible(playbackBar);
    sessionStateStrip_.onFigureClicked = [this](int index) {
        switch (index) {
        case SessionStateStrip::kVersion: showScreen(ScreenType::MixMaster); break;
        case SessionStateStrip::kIssues:
        case SessionStateStrip::kFixCheck: showScreen(ScreenType::Issues); break;
        case SessionStateStrip::kTarget: showScreen(ScreenType::Intake); break;
        default: showScreen(ScreenType::Dashboard); break;
        }
    };
    // RUN-7: the sidebar's readiness follows the verdict card, which refreshes
    // on every issue change (it showed 89 beside the card's 88).
    issues().addIssuesListener(&sessionStateStrip_, [this] { updateSessionStatusLabel(); });
    renders().addRenderTreeListener(&sessionStateStrip_, [this] { refreshSessionStateStrip(); });
    // UX-5: both are session-only chrome; updateSessionChrome() shows them.
    addChildComponent(sessionStateStrip_);
    addChildComponent(abControlStrip_);
    addChildComponent(sessionHealthStrip_);

    // Global intake-progress HUD (spec §9.1): hidden until a pass starts;
    // clicking it jumps to the Dashboard so panels can be watched populate.
    intakeProgressHud_.setVisible(false);
    intakeProgressHud_.setDetailed(reportMode_ == ReportMode::EngineerDetail);
    intakeProgressHud_.onClicked = [this] { showScreen(ScreenType::Dashboard); };
    addChildComponent(intakeProgressHud_);

    reportModeButton_.setTooltip(TRANS("Toggle app-wide detail level. Engineer Detail auto-embeds the "
                                 "Provenance Timeline in the Mix/Master screen; Client Summary hides it."));
    reportModeButton_.onClick = [this]() {
        setReportMode(reportMode_ == ReportMode::EngineerDetail ? ReportMode::ClientSummary
                                                                : ReportMode::EngineerDetail);
    };
    addAndMakeVisible(reportModeButton_);
    applyReportModeButtonStyle();

    // Service-error banner (hidden until an error is surfaced).
    serviceErrorBanner_.setJustificationType(juce::Justification::centred);
    serviceErrorBanner_.setFont(khensu::KhensuTheme::uiFont(khensu::type::secondary, juce::Font::bold));
    serviceErrorBanner_.setVisible(false);
    addChildComponent(serviceErrorBanner_);

    addChildComponent(noticeBar_);
    noticeBar_.onVisibilityChanged = [this] { resized(); };
    postCrashNoticeIfAny();
    wireBounceWatcher(); // ENH-1: after the notice bar it posts to

    // Results gate: covers the result screens while an analysis runs.
    addChildComponent(resultsOverlay_);
    resultsOverlay_.onCancel = [this] { analysis_.cancelAnalysis(); };
    resultsOverlay_.onRerun = [this] { analysis_.reanalyzeActiveSession(); };

    // Render-queue strip: hidden while idle; when it appears/disappears the
    // layout has to reflow so it can claim/release its rows above the transport.
    addChildComponent(renderQueueStrip_);
    renderQueueStrip_.onVisibilityChanged = [this] { resized(); };
    // (The render queue's stem-provenance provider is RenderService's.)
}

void AppController::showMacroMappingWindow()
{
    if (macroMappingWindow_ == nullptr) macroMappingWindow_ = MacroMappingPanel::createWindow(*this);
    macroMappingWindow_->setVisible(true);
    macroMappingWindow_->toFront(true);
}

AppController::~AppController()
{
    stopTimer();
    bounceWatcher_.stop(); // ENH-1: no poll reaches a half-destroyed controller
    analysis_.removeVerificationListener(&bounceWatcher_);
    // Raise isShuttingDown() and drain every pool before anything else is
    // torn down, so a queued/running background job never touches a
    // half-destroyed AppController (BackgroundRunner::shutdown).
    runner_.shutdown();
    smokeVBlank_.reset();
    playback_.removeRealTimeMeterListener(this);
    issues().removeIssuesListener(&sessionStateStrip_);
    renders().removeRenderTreeListener(&sessionStateStrip_);

    // Destroy the overlays and screens NOW, while every controller member is
    // still alive. Their destructors deregister from controller-owned
    // listener containers (analysis_'s service-recovered listeners, meter / vocal /
    // report-mode listeners, the analysis model). Left to implicit member
    // destruction they would run in reverse declaration order, after
    // containers declared below them were already destroyed - the
    // Dashboard's removeServiceRecoveredListener() then erased from a dead
    // std::map: an access violation / heap corruption on every app exit
    // (2026-11-06).
    activeReferenceRitual_.reset();
    activeAbxOverlay_.reset();
    activeMetricDiffOverlay_.reset();
    activeReasoningOverlay_.reset();
    onboardingWizard_.reset();
    activeOverlay.reset();
    aiDetectionScreen.reset();
    settingsScreen.reset();
    referenceLibraryScreen.reset();
    issuesScreen.reset();
    copilotScreen.reset();
    exportScreen.reset();
    systemBrainPanel.reset();
    mixMasterScreen.reset();
    vocalWorkspaceScreen.reset();
    dashboardScreen.reset();
    intakeScreen.reset();

    // Join the bridge's static seek worker while JUCE is still alive (L1).
    ipc::HttpServiceBridge::shutdownBackgroundWorkers();
    lifecycle_.shutdown();

    // Audio pipeline teardown now lives in PlaybackEngine's destructor.

    // Clean up look and feel
    setLookAndFeel(nullptr);
    juce::LookAndFeel::setDefaultLookAndFeel(nullptr); // before lookAndFeel is destroyed
}

void AppController::timerCallback()
{
    ++timerTicks;

    // ---- FEAT-8: the issue playlist's loop (50 ms granularity) ----
    if (loopEnd_ > loopStart_ && playback_.isPlaying()) {
        const double pos = playback_.getPlaybackPosition();
        if (pos >= loopEnd_ || pos < loopStart_ - 0.25) playback_.setPlaybackPosition(loopStart_);
    }

    // ---- Auto-hide the service-error banner after ~6 s ----
    if (serviceErrorBanner_.isVisible() && (juce::Time::getMillisecondCounter() - lastServiceErrorMs_) > 6000) {
        serviceErrorBanner_.setVisible(false);
    }

    // ---- Service health (32 ticks), plugin playhead (4), events (16) ----
    // Before the counter wraps at 600 below, as before (ENG-1 stage C4).
    lifecycle_.tick(timerTicks);

    // ---- FEAT-7: plugin captures waiting to become sessions (every 3 s) ----
    if (timerTicks % 60 == 30) scanPluginCaptures();

    // ---- DB WAL checkpoint (every 600 ticks ≈ 30s) ----
    if (timerTicks % 600 == 0) {
        timerTicks = 0;
        if (auto fm = feedbackMgr) {
            runInBackground([fm]() { fm->checkpoint(false); });
        }
    }

    // UI-7: the Dashboard backdrop no longer draws a playhead, so nothing
    // repaints there while playing (the transport and waveform show position).
}

void AppController::onSmokeVBlank()
{
    if (!isShowing() || !LiquidSmokeBackdrop::shouldAnimate()) return;
    if (!juce::Process::isForegroundProcess()) return;
    const double nowMs = juce::Time::getMillisecondCounterHiRes();
    // ~25 FPS (40 ms) keeps CPU negligible while looking silky at slow smoke speeds.
    if (lastSmokeFrameMs_ > 0.0 && (nowMs - lastSmokeFrameMs_) < 38.0) return;
    const float dt = lastSmokeFrameMs_ > 0.0 ? static_cast<float>((nowMs - lastSmokeFrameMs_) / 1000.0) : 0.04f;
    lastSmokeFrameMs_ = nowMs;
    const float energy = playback_.isPlaying() ? smokeAudioEnergy_ : 0.0f;
    LiquidSmokeBackdrop::instance().advance(dt, energy);
    repaint();
}

void AppController::paint(juce::Graphics& g)
{
    // Aurora Glass backdrop: themed radial wash + subtle static teal/magenta
    // orbs, shared by every screen's glass panels.
    KhensuLookAndFeel::drawAppBackground(g, getLocalBounds());
}

void AppController::resized()
{
    auto b = getLocalBounds();
    if (onboardingWizard_ != nullptr) onboardingWizard_->setBounds(getLocalBounds());

    // Bottom: transport bar (56px, full width)
    playbackBar.setBounds(b.removeFromBottom(KhensuTheme::transportH));

    // Render-queue strip sits directly above the transport, full width, and
    // only takes space while renders are in flight.
    if (renderQueueStrip_.isVisible())
        renderQueueStrip_.setBounds(b.removeFromBottom(renderQueueStrip_.preferredHeight()));

    // Session-health HUD (fatigue coach) sits directly above that.
    if (sessionHealthStrip_.isVisible()) sessionHealthStrip_.setBounds(b.removeFromBottom(SessionHealthStrip::kHeight));

    // Left: sidebar (200px)
    sidebar_.setBounds(b.removeFromLeft(KhensuTheme::sidebarW));

    // Top: Mix Health rollup strip + Report Mode toggle (visible on all screens, 26px height)
    // UI-6: tall enough for the A | B control beside the health figures.
    auto topStrip = b.removeFromTop(kTopStripH);
    reportModeButton_.setBounds(topStrip.removeFromRight(148).reduced(KhensuTheme::sp1, KhensuTheme::sp2));
    if (abControlStrip_.isVisible()) {
        topStrip.removeFromRight(KhensuTheme::sp3);
        abControlStrip_.setBounds(topStrip.removeFromRight(juce::jmin(470, topStrip.getWidth() / 2)));
    }
    sessionStateStrip_.setBounds(topStrip);

    // Global intake-progress HUD (spec §9.1): pinned above the content on
    // every screen while active; zero-height when hidden.
    if (intakeProgressHud_.isVisible()) intakeProgressHud_.setBounds(b.removeFromTop(intakeProgressHud_.preferredHeight()));

    // Crash / update notices take a row only while one is showing.
    if (noticeBar_.isVisible()) noticeBar_.setBounds(b.removeFromTop(AppNoticeBar::kHeight));

    // Remaining: content area
    contentContainer.setBounds(b);

    // ComponentAnimator::fadeIn() animates bounds as well as alpha, towards the
    // bounds the screen had when the fade began. The launch screen is faded in
    // before the window's first layout, i.e. towards 0x0, so without this the
    // animator kept shrinking Intake back to nothing after the layout below and
    // the launch page came up empty. Finish any fade (alpha -> 1) first; the
    // setBounds calls below then win.
    if (screenAnimator_.isAnimating()) screenAnimator_.cancelAllAnimations(true);

    // Active Screen layout inside Content Container
    intakeScreen->setBounds(contentContainer.getLocalBounds());
    dashboardScreen->setBounds(contentContainer.getLocalBounds());
    vocalWorkspaceScreen->setBounds(contentContainer.getLocalBounds());
    mixMasterScreen->setBounds(contentContainer.getLocalBounds());
    systemBrainPanel->setBounds(helpDrawerBounds()); // CUT-4: a drawer, not a screen
    exportScreen->setBounds(contentContainer.getLocalBounds());
    if (copilotScreen != nullptr) copilotScreen->setBounds(contentContainer.getLocalBounds());
    issuesScreen->setBounds(contentContainer.getLocalBounds());
    if (referenceLibraryScreen != nullptr) referenceLibraryScreen->setBounds(contentContainer.getLocalBounds());
    settingsScreen->setBounds(contentContainer.getLocalBounds());
    if (aiDetectionScreen != nullptr) aiDetectionScreen->setBounds(contentContainer.getLocalBounds());

    // The results gate covers the whole content area; the error banner and
    // the intake HUD stay above it.
    resultsOverlay_.setBounds(contentContainer.getBounds());
    resultsOverlay_.toFront(false);
    layoutCoach(); // UX-15: above the overlay
    if (coachCard_ != nullptr) coachCard_->toFront(false);

    // Error banner overlays the top of the content area.
    serviceErrorBanner_.setBounds(contentContainer.getBounds().removeFromTop(28));
    serviceErrorBanner_.toFront(false);

    // The intake HUD sits above the content strip - keep it on top of the
    // screen components that share the region.
    if (intakeProgressHud_.isVisible()) intakeProgressHud_.toFront(false);

    // Command palette (when open) covers the whole window.
    commandPaletteController_.resized(getLocalBounds());
}

void AppController::showServiceError(const juce::String& message)
{
    const juce::int64 now = juce::Time::getMillisecondCounter();
    // De-duplicate identical errors within 5 s (pollers can fire repeatedly).
    if (message == lastServiceError_ && (now - lastServiceErrorMs_) < 5000) return;
    lastServiceError_ = message;
    lastServiceErrorMs_ = now;

    serviceErrorBanner_.setText(juce::String(juce::CharPointer_UTF8("  \xe2\x9a\xa0  ")) + message, juce::dontSendNotification);
    serviceErrorBanner_.setVisible(true);
    serviceErrorBanner_.toFront(false);
    resized();
}

bool AppController::keyPressed(const juce::KeyPress& key)
{
    // Ctrl/Cmd+K - toggle the command palette. Handled before the other
    // Cmd chords so it works regardless of which screen has focus.
    if (commandPaletteController_.handleKeyPress(key)) return true;

    const int code = key.getKeyCode();
    if (code == juce::KeyPress::escapeKey && helpDrawerOpen_) { // CUT-4
        closeHelpDrawer();
        return true;
    }
    if (code == juce::KeyPress::F6Key) {
        cycleFocusRegion(key.getModifiers().isShiftDown());
        return true;
    }
    if (code == juce::KeyPress::F1Key) {
        showKeyboardHelp();
        return true;
    }
    if (key.getModifiers().isCommandDown() && !key.getModifiers().isShiftDown() && !key.getModifiers().isAltDown()) {
        // Ctrl+1..9 / Ctrl+0: the Nth sidebar screen.
        if (code >= '0' && code <= '9') {
            const int visual = code == '0' ? 9 : code - '1';
            if (visual < SidebarComponent::kNumScreens) {
                showScreen(SidebarComponent::screenTypeForVisualIndex(visual));
                return true;
            }
        }
        if (code == juce::KeyPress::spaceKey) {
            if (playback_.isPlaying())
                playback_.pauseAudio();
            else
                playback_.playAudio();
            return true;
        }
        // UX-11: the core verbs.
        switch (code) {
        case 'O': runCoreVerb(CoreVerb::OpenFile); return true;
        case 'R': runCoreVerb(CoreVerb::Analyse); return true;
        case 'E': runCoreVerb(CoreVerb::Export); return true;
        case ',': runCoreVerb(CoreVerb::Settings); return true;
        default: break;
        }
    }
    // A bare key reaches here only when the focused component didn't consume
    // it (a text field takes its characters first), so Space plays and B / F
    // act outside text entry (UX-11).
    if (!key.getModifiers().isAnyModifierKeyDown() && hasActiveSession() && !textInputHasFocus()) {
        if (code == juce::KeyPress::spaceKey) {
            if (playback_.isPlaying())
                playback_.pauseAudio();
            else
                playback_.playAudio();
            return true;
        }
        if (code == 'B') {
            runCoreVerb(CoreVerb::ToggleAb);
            return true;
        }
        if (code == 'F') {
            runCoreVerb(CoreVerb::FixTopIssue);
            return true;
        }
    }
    // ENH-2: Shift+F fixes every safe issue (outside text entry, as F does).
    if (code == 'F' && key.getModifiers().isShiftDown() && !key.getModifiers().isCommandDown() &&
        !key.getModifiers().isAltDown() && !key.getModifiers().isCtrlDown() && hasActiveSession() &&
        !textInputHasFocus()) {
        runCoreVerb(CoreVerb::FixAllSafe);
        return true;
    }
    if (key.getModifiers().isCommandDown()) {
        // BUG-3: Ctrl+Shift+Z is redo (as in the plugin); Ctrl+Y stays redo.
        if (key.getKeyCode() == 'Z' && key.getModifiers().isShiftDown()) {
            if (canRedo()) {
                redo();
                return true;
            }
        } else if (key.getKeyCode() == 'Z') {
            if (canUndo()) {
                undo();
                return true;
            }
        } else if (key.getKeyCode() == 'Y') {
            if (canRedo()) {
                redo();
                return true;
            }
        }
    } else if (key.getKeyCode() == 'N' && hasActiveSession() && !key.getModifiers().isCommandDown() &&
               !key.getModifiers().isCtrlDown() && !key.getModifiers().isAltDown() &&
               !key.getModifiers().isShiftDown()) {
        // Keyboard-driven "next issue" - cycles unresolved recommendations
        // by severity. No modifier so it doesn't fight Cmd+N/Ctrl+N (New);
        // JUCE only routes here once no focused text-input component claims it.
        focusNextIssue();
        return true;
    }
    return juce::Component::keyPressed(key);
}

bool AppController::isInterestedInFileDrag(const juce::StringArray& files)
{
    return !app_file_drop::audioFiles(files).empty();
}

void AppController::fileDragEnter(const juce::StringArray&, int, int)
{
    fileDragOver_ = true;
    repaint();
}

void AppController::fileDragExit(const juce::StringArray&)
{
    fileDragOver_ = false;
    repaint();
}

void AppController::filesDropped(const juce::StringArray& files, int, int)
{
    fileDragOver_ = false;
    repaint();
    const auto audio = app_file_drop::audioFiles(files);
    if (audio.empty() || intakeScreen == nullptr) return;
    // The same path as a drop on Intake's zone: probe, intake scan, then the
    // user confirms and creates the session there.
    showScreen(ScreenType::Intake);
    intakeScreen->dropFilesForDiagnostics(audio);
}

void AppController::paintOverChildren(juce::Graphics& g)
{
    if (!fileDragOver_) return;
    const auto area = contentContainer.getBounds().toFloat().reduced(12.0f);
    g.setColour(KhensuTheme::scrim);
    g.fillRect(getLocalBounds());
    g.setColour(KhensuTheme::accentTealDim);
    g.fillRoundedRectangle(area, 12.0f);
    g.setColour(KhensuTheme::accentTeal);
    const float dash[] = {8.0f, 6.0f};
    juce::Path outline;
    outline.addRoundedRectangle(area, 12.0f);
    juce::Path dashed;
    juce::PathStrokeType(2.0f).createDashedStroke(dashed, outline, dash, 2);
    g.fillPath(dashed); // a dashed stroke is a filled outline (CLAUDE.md gotcha)
    g.setColour(KhensuTheme::textPrimary);
    g.setFont(KhensuTheme::uiFont(KhensuTheme::kFontH1, juce::Font::bold));
    g.drawText(TRANS("Drop to open this track"), area.toNearestInt().withTrimmedBottom(28),
               juce::Justification::centred, true);
    g.setColour(KhensuTheme::textSecondary);
    g.setFont(KhensuTheme::bodyFont());
    g.drawText(TRANS("WAV, AIFF, FLAC, MP3, M4A or OGG - it opens on Intake as a new session"),
               area.toNearestInt().withTrimmedTop(28), juce::Justification::centred, true);
}

bool AppController::textInputHasFocus()
{
    return dynamic_cast<juce::TextEditor*>(juce::Component::getCurrentlyFocusedComponent()) != nullptr;
}

bool AppController::canUndo() const noexcept
{
    return vocalEdit_.canUndo() || renders_.canUndoRender();
}

bool AppController::canRedo() const noexcept
{
    return vocalEdit_.canRedo() || renders_.canRedoRender();
}

void AppController::undo()
{
    const bool vocalFirst = currentScreen == ScreenType::VocalWorkspace;
    if (vocalFirst && vocalEdit_.canUndo()) return vocalEdit_.undo();
    if (renders_.canUndoRender()) return renders_.undoRender(); // the default (WF-13)
    if (vocalEdit_.canUndo()) vocalEdit_.undo();
}

void AppController::redo()
{
    const bool vocalFirst = currentScreen == ScreenType::VocalWorkspace;
    if (vocalFirst && vocalEdit_.canRedo()) return vocalEdit_.redo();
    if (renders_.canRedoRender()) return renders_.redoRender();
    if (vocalEdit_.canRedo()) vocalEdit_.redo();
}

void AppController::maybeShowCoach()
{
    if (suppressCoach_ || coachCard_ != nullptr) return;
    if (auto done = sessionManager().getUserPreference("first_session_coach");
        done.is_ok() && done.value() == "done")
        return;
    coachCard_ = std::make_unique<CoachMarkCard>();
    coachCard_->onRunStep = [this](CoachMarkCard::Step step) {
        // The card itself ticks the step; runCoreVerb's markDone is a no-op then.
        runCoreVerb(step == CoachMarkCard::FixTopIssue ? CoreVerb::FixTopIssue
                    : step == CoachMarkCard::HearAb    ? CoreVerb::ToggleAb
                                                       : CoreVerb::Export);
    };
    coachCard_->onFinished = [this] {
        sessionManager().setUserPreference("first_session_coach", "done");
        // Not deleted inside its own click handler: on the next turn.
        juce::Component::SafePointer<AppController> self(this);
        juce::MessageManager::callAsync([self] {
            if (self != nullptr) self->coachCard_.reset();
        });
    };
    addAndMakeVisible(*coachCard_);
    layoutCoach();
    coachCard_->toFront(false);
}

void AppController::layoutCoach()
{
    if (coachCard_ == nullptr) return;
    const auto area = contentContainer.getBounds().reduced(16);
    coachCard_->setBounds(juce::Rectangle<int>(area.getRight() - coachCard_->getWidth(),
                                               area.getBottom() - coachCard_->getHeight(),
                                               coachCard_->getWidth(), coachCard_->getHeight()));
}

void AppController::showIssue(const std::string& key)
{
    showScreen(ScreenType::Issues);
    if (issuesScreen) issuesScreen->selectIssue(key);
}

bool AppController::revealCardForFinding(const std::string& findingKeyOrCode)
{
    // ENH-7: only switch screens when a card shows the finding.
    if (dashboardScreen == nullptr || finding_card::forFinding(findingKeyOrCode).empty()) return false;
    showScreen(ScreenType::Dashboard);
    return dashboardScreen->revealCardForFinding(findingKeyOrCode);
}

// ---- STU-7: what the command palette finds besides commands ------------------------

std::vector<ICommandPaletteHost::SearchTarget> AppController::searchableCards()
{
    std::vector<SearchTarget> out;
    if (dashboardScreen != nullptr)
        for (auto& card : dashboardScreen->searchableCards())
            out.push_back({card.title, card.keywords, card.id});
    return out;
}

void AppController::revealCard(const std::string& cardId)
{
    showScreen(ScreenType::Dashboard);
    // Without a session the screen opens; there is no card to reveal yet.
    if (dashboardScreen != nullptr) juce::ignoreUnused(dashboardScreen->revealCard(cardId));
}

std::vector<ICommandPaletteHost::SearchTarget> AppController::searchableFindings()
{
    std::vector<SearchTarget> out;
    if (!hasAnalysis()) return out;
    for (const auto& issue : issues_.list()) {
        if (issue.settled()) continue; // open findings only
        const std::string key = issue.recId.empty() ? issue.code : issue.recId;
        if (key.empty()) continue;
        juce::String keywords = juce::String(issue.code).replaceCharacter('_', ' ');
        keywords << " " << juce::String(issue.rec.stem_or_bus) << " " << juce::String(issue.rec.section) << " " << IssueQueuePanel::severityWord(issue.rec.severity)
                 << " finding issue problem";
        out.push_back({IssueQueuePanel::titleFor(issue), keywords, key});
    }
    return out;
}

std::vector<ICommandPaletteHost::SearchTarget> AppController::searchableGenres()
{
    std::vector<SearchTarget> out;
    const auto& db = genreDatabase();
    out.reserve(db.listAll().size());
    for (const auto& g : db.listAll()) {
        juce::String keywords = juce::String(g.id).replaceCharacter('-', ' ');
        if (!g.parent.empty())
            if (const auto parent = db.findById(g.parent)) keywords << " " << juce::String(parent->name);
        keywords << " genre style target";
        out.push_back({juce::String(g.name.empty() ? g.id : g.name), keywords, g.id});
    }
    return out;
}

void AppController::showGenre(const std::string& genreId)
{
    // Non-destructive: the genre's profile in the Reference Library, not a
    // new target for the session. Shown first, so the screen's first-visit
    // default pick does not replace this one.
    showScreen(ScreenType::ReferenceLibrary);
    if (referenceLibraryScreen != nullptr) referenceLibraryScreen->selectGenre(juce::String(genreId));
}

void AppController::runCoreVerb(CoreVerb verb)
{
    switch (verb) {
    case CoreVerb::OpenFile:
        showScreen(ScreenType::Intake);
        if (intakeScreen) intakeScreen->browseForFiles();
        return;
    case CoreVerb::Analyse:
        if (hasActiveSession())
            analysis_.reanalyzeActiveSession();
        else
            runCoreVerb(CoreVerb::OpenFile);
        return;
    case CoreVerb::FixTopIssue:
        // CON-1: fixed in the issue queue, where its check then shows.
        if (!hasAnalysis()) return;
        showScreen(ScreenType::Issues);
        if (issuesScreen) issuesScreen->fixTopIssue();
        if (coachCard_) coachCard_->markDone(CoachMarkCard::FixTopIssue); // UX-15
        return;
    case CoreVerb::FixAllSafe:
        // ENH-2: one version from every validated fix, checked like any fix.
        if (!hasAnalysis()) return;
        showScreen(ScreenType::Issues);
        if (issuesScreen) issuesScreen->fixAllSafe();
        return;
    case CoreVerb::ToggleAb:
        if (playback_.hasAbComparison()) {
            playback_.toggleAb();
            if (coachCard_) coachCard_->markDone(CoachMarkCard::HearAb);
        }
        return;
    case CoreVerb::Export:
        showScreen(ScreenType::Export);
        if (coachCard_) coachCard_->markDone(CoachMarkCard::Export);
        return;
    case CoreVerb::Settings:
        showScreen(ScreenType::Settings);
        return;
    }
}

void AppController::loopRegion(double startSec, double endSec)
{
    if (endSec <= startSec) return stopLoopRegion();
    loopStart_ = startSec;
    loopEnd_ = endSec;
    playback_.setPlaybackPosition(startSec);
    if (!playback_.isPlaying()) playback_.playAudio();
}

void AppController::stopLoopRegion()
{
    loopStart_ = loopEnd_ = 0.0;
}

bool AppController::isLoopingRegion(double startSec, double endSec) const noexcept
{
    return loopEnd_ > loopStart_ && std::abs(loopStart_ - startSec) < 1e-3 && std::abs(loopEnd_ - endSec) < 1e-3;
}

void AppController::focusNextIssue()
{
    showScreen(ScreenType::MixMaster);
    if (mixMasterScreen) mixMasterScreen->focusNextIssue();
}

void AppController::enqueueProvenanceEvent(const juce::String& sessionId, const juce::String& eventType,
                                            const juce::String& summary, const juce::String& recommendationId,
                                            const juce::var& payload)
{
    if (!sessions_.sharedManager()) return;

    auto* d = new juce::DynamicObject();
    d->setProperty("session_id", sessionId);
    d->setProperty("event_type", eventType);
    d->setProperty("summary", summary);
    d->setProperty("rec_id", recommendationId);
    d->setProperty("payload", payload);
    sessions_.manager().enqueueOutboxEvent((juce::Uuid().toString() + "-event").toStdString(), "event",
                                   juce::JSON::toString(juce::var(d), false).toStdString());
    lifecycle_.dispatchOutboxIfNeeded();
}

void AppController::setReportMode(ReportMode mode)
{
    if (mode == reportMode_) return;
    reportMode_ = mode;
    // UI-10: the six-stage strip is engineer detail; otherwise one calm line.
    intakeProgressHud_.setDetailed(reportMode_ == ReportMode::EngineerDetail);
    applyReportModeButtonStyle();
    notifyKeyedListeners(reportModeListeners_, reportMode_);
}

void AppController::addReportModeListener(void* key, std::function<void(ReportMode)> cb)
{
    reportModeListeners_[key] = std::move(cb);
}

void AppController::removeReportModeListener(void* key) { reportModeListeners_.erase(key); }

/// Colours reportModeButton_ so the current mode is glanceable without reading
/// the label. Theme tokens, not literals (UI-01); re-run on every mode change,
/// so it also re-resolves the colour after a theme switch.
void AppController::applyReportModeButtonStyle()
{
    const bool engineer = reportMode_ == ReportMode::EngineerDetail;
    reportModeButton_.setButtonText(engineer ? TRANS("Engineer Detail") : TRANS("Client Summary"));
    // UX2-3: a neutral secondary button - the label says the mode; amber /
    // teal fills read as a warning and a second accent.
    reportModeButton_.removeColour(juce::TextButton::buttonColourId);
    reportModeButton_.removeColour(juce::TextButton::textColourOffId);
}

std::vector<std::pair<juce::String, std::function<void()>>> AppController::navigationCommands()
{
    std::vector<std::pair<juce::String, std::function<void()>>> result;
    result.reserve(SidebarComponent::kNumScreens);
    for (int i = 0; i < SidebarComponent::kNumScreens; ++i) {
        const juce::String label = SidebarComponent::navLabelForVisualIndex(i);
        const ScreenType dest = SidebarComponent::screenTypeForVisualIndex(i);
        result.push_back({label, [this, dest]() { showScreen(dest); }});
    }
    return result;
}

void AppController::toggleReportMode()
{
    setReportMode(reportMode_ == ReportMode::EngineerDetail ? ReportMode::ClientSummary
                                                            : ReportMode::EngineerDetail);
}

void AppController::cycleTheme()
{
    juce::ignoreUnused(KhensuTheme::cycleTheme());
    if (auto* top = getTopLevelComponent()) top->repaint();
    repaint();
}

template <typename ScreenT>
juce::Component* AppController::ensureLazyScreen(std::unique_ptr<ScreenT>& slot)
{
    if (slot == nullptr) {
        slot = std::make_unique<ScreenT>(*this);
        contentContainer.addChildComponent(slot.get());
        slot->setBounds(contentContainer.getLocalBounds());
        AccessibilityCoverage::apply(*slot);
    }
    return slot.get();
}

juce::Component* AppController::ensureScreenBuilt(ScreenType screen)
{
    switch (screen) {
    case ScreenType::Copilot:
        return ensureLazyScreen(copilotScreen);
    case ScreenType::ReferenceLibrary:
        return ensureLazyScreen(referenceLibraryScreen);
    case ScreenType::AiDetection:
        return ensureLazyScreen(aiDetectionScreen);
    default:
        return nullptr;
    }
}

void AppController::showScreen(ScreenType screen)
{
    // CUT-4: Help opens (or, pressed again, closes) the drawer over the
    // current screen; any other screen closes it.
    if (screen == ScreenType::SystemBrain) {
        if (helpDrawerOpen_)
            closeHelpDrawer();
        else
            openHelpDrawer();
        return;
    }
    if (helpDrawerOpen_) closeHelpDrawer();
    juce::Component* selectedScreen = nullptr;

    // Cancel an interrupted transition before changing the visible screen.
    // Hidden screens are restored to full alpha so the next transition always
    // starts from a known state.
    screenAnimator_.cancelAllAnimations(false);
    intakeScreen->setVisible(false);
    intakeScreen->setAlpha(1.0f);
    dashboardScreen->setVisible(false);
    dashboardScreen->setAlpha(1.0f);
    vocalWorkspaceScreen->setVisible(false);
    vocalWorkspaceScreen->setAlpha(1.0f);
    mixMasterScreen->setVisible(false);
    mixMasterScreen->setAlpha(1.0f);
    systemBrainPanel->setVisible(false);
    systemBrainPanel->setAlpha(1.0f);
    exportScreen->setVisible(false);
    exportScreen->setAlpha(1.0f);
    if (copilotScreen != nullptr) {
        copilotScreen->setVisible(false);
        copilotScreen->setAlpha(1.0f);
    }
    issuesScreen->setVisible(false);
    issuesScreen->setAlpha(1.0f);
    if (referenceLibraryScreen != nullptr) {
        referenceLibraryScreen->setVisible(false);
        referenceLibraryScreen->setAlpha(1.0f);
    }
    settingsScreen->setVisible(false);
    settingsScreen->setAlpha(1.0f);
    if (aiDetectionScreen != nullptr) {
        aiDetectionScreen->setVisible(false);
        aiDetectionScreen->setAlpha(1.0f);
    }

    currentScreen = screen;
    crash::setCurrentScreen(crashScreenName(screen)); // recorded in a crash report's summary
    switch (currentScreen) {
    case ScreenType::Intake:
        selectedScreen = intakeScreen.get();
        break;
    case ScreenType::Dashboard:
        selectedScreen = dashboardScreen.get();
        break;
    case ScreenType::VocalWorkspace:
        selectedScreen = vocalWorkspaceScreen.get();
        break;
    case ScreenType::MixMaster:
        selectedScreen = mixMasterScreen.get();
        break;
    case ScreenType::SystemBrain: // the help drawer, handled at the top (CUT-4)
        break;
    case ScreenType::Export:
        selectedScreen = exportScreen.get();
        break;
    case ScreenType::Copilot:
        selectedScreen = ensureLazyScreen(copilotScreen);
        break;
    case ScreenType::Issues:
        selectedScreen = issuesScreen.get();
        break;
    case ScreenType::ReferenceLibrary:
        selectedScreen = ensureLazyScreen(referenceLibraryScreen);
        break;
    case ScreenType::Settings:
        selectedScreen = settingsScreen.get();
        break;
    case ScreenType::AiDetection:
        selectedScreen = ensureLazyScreen(aiDetectionScreen);
        break;
    }

    if (selectedScreen != nullptr) {
        // The fade targets the screen's current bounds (see resized()), so
        // make sure they are the content area's before it starts.
        if (!contentContainer.getLocalBounds().isEmpty())
            selectedScreen->setBounds(contentContainer.getLocalBounds());
        selectedScreen->setVisible(true);
        selectedScreen->toFront(false);
        if (ui::motion::reduced()) {
            selectedScreen->setAlpha(1.0f); // VIS-7: no fade
        } else {
            selectedScreen->setAlpha(0.0f);
            screenAnimator_.fadeIn(selectedScreen, 180);
        }
        // Every interactive control gets a spoken name and tooltip, including
        // rows built since the screen was last shown (see AccessibilityCoverage).
        AccessibilityCoverage::apply(*selectedScreen);
    }

    sidebar_.setActiveScreen(SidebarComponent::visualIndexForScreenType(screen));
    updateResultsOverlay();
}

juce::Rectangle<int> AppController::helpDrawerBounds() const
{
    const auto area = contentContainer.getLocalBounds();
    const int w = juce::jlimit(juce::jmin(420, area.getWidth()), area.getWidth(), area.getWidth() * 55 / 100);
    return area.withTrimmedLeft(area.getWidth() - juce::jmin(w, 820));
}

void AppController::openHelpDrawer()
{
    if (systemBrainPanel == nullptr) return;
    systemBrainPanel->updateState();
    systemBrainPanel->setAlpha(1.0f);
    systemBrainPanel->setBounds(helpDrawerBounds());
    systemBrainPanel->setVisible(true);
    systemBrainPanel->toFront(true);
    AccessibilityCoverage::apply(*systemBrainPanel);
    if (!helpDrawerOpen_) crash::setCurrentScreen(crashScreenName(ScreenType::SystemBrain));
    helpDrawerOpen_ = true;
    sidebar_.setActiveScreen(SidebarComponent::visualIndexForScreenType(ScreenType::SystemBrain));
}

void AppController::closeHelpDrawer()
{
    if (systemBrainPanel != nullptr) systemBrainPanel->setVisible(false);
    helpDrawerOpen_ = false;
    crash::setCurrentScreen(crashScreenName(currentScreen));
    sidebar_.setActiveScreen(SidebarComponent::visualIndexForScreenType(currentScreen));
}

void AppController::cycleFocusRegion(bool backwards)
{
    // F6 regions, in visual order: sidebar navigation, the active screen,
    // the transport bar.
    juce::Component* regions[] = {&sidebar_, &contentContainer, &playbackBar};
    int current = -1;
    if (auto* focused = juce::Component::getCurrentlyFocusedComponent())
        for (int i = 0; i < 3; ++i)
            if (regions[i] == focused || regions[i]->isParentOf(focused)) current = i;
    const int next = current < 0 ? 0 : (current + (backwards ? 2 : 1)) % 3;
    auto* region = regions[next];
    if (auto traverser = region->createKeyboardFocusTraverser())
        if (auto* first = traverser->getDefaultComponent(region)) {
            first->grabKeyboardFocus();
            return;
        }
    region->grabKeyboardFocus();
}

void AppController::showKeyboardHelp()
{
    showExplainer(TRANS("Keyboard shortcuts"), TRANS("Accessibility"),
                  TRANS("Ctrl+K  command palette (every screen and action)\n"
                  "Ctrl+1..9, Ctrl+0  go to the Nth screen in the sidebar\n"
                  "F6 / Shift+F6  move between sidebar, screen, and transport\n"
                  "Tab / Shift+Tab  next / previous control;  Return or Space activates\n"
                  "Space or Ctrl+Space  play / pause\n"
                  "Ctrl+O  open an audio file;  Ctrl+R  analyse again\n"
                  "F  fix the top issue;  Shift+F  fix all safe issues;  B  switch A/B (original / fix)\n"
                  "Ctrl+E  export;  Ctrl+,  settings\n"
                  "Ctrl+Z / Ctrl+Y or Ctrl+Shift+Z  undo / redo: the focused editor, else the render history (vocal edits on Vocal)\n"
                  "Drop an audio file on any screen  open it as a session\n"
                  "N  next unresolved issue\n"
                  "Esc  close a dialog or overlay\n"
                  "F1  this list"));
}

void AppController::showExplainer(const juce::String& title, const juce::String& category,
                                  const juce::String& description, const juce::String& systemBrainFeatureId)
{
    // Create overlay
    activeOverlay = std::make_unique<ExplainerOverlay>(title, category, description, systemBrainFeatureId);
    activeOverlay->onDismiss = [this]() { hideExplainer(); };
    activeOverlay->onViewSystemBrain = [this, systemBrainFeatureId]() {
        hideExplainer();
        showSystemBrainFeature(systemBrainFeatureId);
    };
    addAndMakeVisible(activeOverlay.get());

    // Position it
    activeOverlay->setBounds(getLocalBounds());
}

void AppController::showSystemBrainFeature(const juce::String& featureId)
{
    openHelpDrawer();
    if (systemBrainPanel != nullptr) {
        systemBrainPanel->selectFeature(featureId.toStdString());
    }
}

void AppController::showMixMasterCard(int tabIdx)
{
    showScreen(ScreenType::MixMaster);
    if (mixMasterScreen != nullptr) {
        mixMasterScreen->jumpToCard(tabIdx);
    }
}

void AppController::showExportStep(int step)
{
    showScreen(ScreenType::Export);
    if (exportScreen != nullptr) exportScreen->showStep(step);
}

void AppController::hideExplainer()
{
    if (activeOverlay != nullptr) {
        removeChildComponent(activeOverlay.get());
        activeOverlay.reset();
    }
}

void AppController::showChainReasoning(const Recommendation& rec)
{
    activeReasoningOverlay_ = std::make_unique<ChainReasoningOverlay>(rec);
    activeReasoningOverlay_->onDismiss = [this]() { hideChainReasoning(); };
    addAndMakeVisible(activeReasoningOverlay_.get());
    activeReasoningOverlay_->setBounds(getLocalBounds());

    // "In plain words" + myth check from /copilot/explain: an LLM when one is
    // configured, otherwise the service's deterministic text_factory narrative
    // with myth_guardrail context. Best-effort - the trace above stands alone.
    const std::string fileId = hasAnalysis() ? currentAnalysis().file_id : std::string{};
    const juce::String issueCode(issueCodeOf(rec, fileId));
    const juce::String what(rec.what), action(rec.suggested_action), why(rec.why_helps), note(rec.engineer_note);
    const float severity01 = juce::jlimit(0.0f, 1.0f, rec.severity / 100.0f);
    juce::String genre;
    double bpm = 0.0;
    if (hasAnalysis()) {
        genre = juce::String(currentAnalysis().python_data.genre_detected);
        bpm = currentAnalysis().python_data.bpm_detected;
    }
    juce::Component::SafePointer<ChainReasoningOverlay> overlay(activeReasoningOverlay_.get());
    runInBackground([overlay, issueCode, what, action, why, note, severity01, genre, bpm]() {
        ipc::HttpServiceBridge bridge;
        auto result = bridge.explainRecommendation(issueCode, what, action, severity01, why, note, genre, bpm);
        if (result.is_err()) return;
        juce::MessageManager::callAsync([overlay, r = result.value()]() {
            if (overlay == nullptr) return; // closed meanwhile
            overlay->setPlainLanguage(r.explanation, r.isMythAdjacent ? r.mythClarification : juce::String(),
                                      r.mostLikelyCause, r.tradeOff, r.howToVerify);
        });
    });
}

void AppController::hideChainReasoning()
{
    if (activeReasoningOverlay_ != nullptr) {
        removeChildComponent(activeReasoningOverlay_.get());
        activeReasoningOverlay_.reset();
    }
}

void AppController::showReferenceTrackRitual()
{
    if (activeReferenceRitual_ != nullptr) return;

    auto* overlay = new ReferenceTrackRitualOverlay();
    activeReferenceRitual_.reset(overlay);
    overlay->onClose = [this] { hideReferenceTrackRitual(); };
    overlay->onReferenceDropped = [this](const juce::String& path) { analysis_.setReferenceTrack(path); };
    overlay->onReferenceCleared = [this]() { analysis_.setReferenceTrack({}); };
    overlay->onApplyMatchEq = [this](const juce::String& /*path*/) {
        // Match-EQ render path: trigger ReferenceComparePanel's existing apply.
        if (dashboardScreen) dashboardScreen->applyReferenceMatchEq();
    };
    overlay->onAbCompare = [this]() { playback_.toggleAb(); };

    // Seed with any existing reference path.
    overlay->setCurrentReference(analysis_.getReferenceTrack());

    addAndMakeVisible(overlay);
    overlay->setBounds(getLocalBounds());
    overlay->toFront(true);
    overlay->grabKeyboardFocus();
}

void AppController::hideReferenceTrackRitual()
{
    if (activeReferenceRitual_ != nullptr) {
        removeChildComponent(activeReferenceRitual_.get());
        activeReferenceRitual_.reset();
    }
}

void AppController::openDemoSessionOnFirstRun()
{
    // --screenshot / --ui-bench / --a11y-audit own the session (and the store's first run).
    for (const auto& a : juce::JUCEApplicationBase::getCommandLineParameterArray())
        if (a.startsWith("--screenshot") || a.startsWith("--ui-bench") || a.startsWith("--a11y-audit")
            || a.startsWith("--soak-audio"))
            return;
    auto offered = sessionManager().getUserPreference("demo_session_offered");
    if (offered.is_ok() && !offered.value().empty()) return;
    sessionManager().setUserPreference("demo_session_offered", "1");
    const auto file = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                          .getChildFile("KhensuMixing")
                          .getChildFile("demo")
                          .getChildFile("Khensu Demo Song.wav");
    juce::Component::SafePointer<AppController> self(this);
    runInBackground([self, file] {
        const bool ok = file.existsAsFile() || demo_song::write(file);
        juce::MessageManager::callAsync([self, file, ok] {
            if (self == nullptr || !ok || self->hasActiveSession()) return;
            AudioFileRef ref;
            ref.id = juce::Uuid().toString().toStdString();
            ref.path = file.getFullPathName().toStdString();
            ref.format = "wav";
            CreativeTarget target;
            target.primary_genre = "synthpop";
            target.playback_intent = "streaming";
            if (self->startSession(TRANS("Demo song"), {ref}, target)) {
                AppNoticeBar::Notice n;
                n.id = "demo_session";
                n.text = TRANS("This is a demo song, made by Khensu to show what the analysis finds. Load your own "
                               "track with Open (Ctrl+O).");
                self->noticeBar_.post(std::move(n));
            }
        });
    });
}

void AppController::refreshSessionStateStrip()
{
    SessionStateStrip::State st;
    st.hasSession = hasActiveSession();
    if (st.hasSession) {
        st.versionNode = renders().currentRenderTreeNode();
        st.versionCount = static_cast<int>(renders().renderTree().size());
        for (const auto& i : issues().list()) {
            if (i.check == IIssues::Check::Resolved) ++st.resolved;
            else if (i.check == IIssues::Check::Improved) ++st.improved;
            else if (i.check == IIssues::Check::Worse || i.check == IIssues::Check::New) ++st.worse;
            if (i.check != IIssues::Check::None) st.checked = true;
            if (i.settled() || i.rec.type == RecommendationType::Strength) continue;
            if (i.rec.severity >= 66.0f) ++st.severe;
            else if (i.rec.severity >= 33.0f) ++st.moderate;
            else ++st.minor;
        }
        if (const auto* s = getActiveSession()) {
            // BUG-1: the genre the findings were judged against - the stated
            // one, the detected one (said so), or the generic target.
            const auto genre = genre_in_use::resolve(s->target.primary_genre,
                                                     hasAnalysis() ? &currentAnalysis() : nullptr,
                                                     analysis_.genreDatabase());
            juce::StringArray parts;
            parts.add(genre_in_use::label(genre));
            if (!s->target.playback_intent.empty())
                parts.add(juce::String(s->target.playback_intent).substring(0, 1).toUpperCase()
                          + juce::String(s->target.playback_intent).substring(1));
            st.target = parts.joinIntoString(", ");
        }
        st.analysis = analysisState_ == SessionStateStrip::State::Analysis::None && hasAnalysis()
                          ? SessionStateStrip::State::Analysis::Ready
                          : analysisState_;
    }
    sessionStateStrip_.setState(st);
}

void AppController::updateSessionStatusLabel()
{
    refreshSessionStateStrip();
    if (hasActiveSession()) {
        auto* s = getActiveSession();
        // BUG-1: the genre's display name (UX2-4), the detected one said so.
        juce::String genre;
        if (s != nullptr)
            genre = genre_in_use::label(genre_in_use::resolve(s->target.primary_genre,
                                                              hasAnalysis() ? &currentAnalysis() : nullptr,
                                                              analysis_.genreDatabase()));
        sidebar_.setSessionName(s ? juce::String(s->name) : juce::String(), genre);
        sidebar_.setAnalysisStatus(hasAnalysis() ? TRANS("Ready") : TRANS("Pending"), hasAnalysis());
        // UX2-4: the session block's readiness - the verdict's one number.
        const bool scored = hasAnalysis() && !currentAnalysis().is_partial;
        sidebar_.setReadiness(scored ? analysis_.derivedResultData().scorecard.overall_score : 0.0f, scored);
    } else {
        sidebar_.setSessionName({}, {});
        sidebar_.setAnalysisStatus(TRANS("No session"), false);
        sidebar_.setReadiness(0.0f, false);
    }
    updateSessionChrome();
}

void AppController::selectSystemBrainFeature(const std::string& id)
{
    if (systemBrainPanel != nullptr) {
        systemBrainPanel->selectFeature(id);
        openHelpDrawer();
    }
}

void AppController::handleShowReasoningEvent(const juce::String& recId)
{
    if (auto* mainWin = juce::TopLevelWindow::getTopLevelWindow(0)) {
        mainWin->toFront(true);
        juce::Process::makeForegroundProcess();
    }
    if (recId.isNotEmpty() && hasAnalysis()) {
        for (const auto& rec : currentAnalysis().recommendations) {
            if (rec.id == recId.toStdString()) {
                showChainReasoning(rec);
                return;
            }
        }
    }
    showScreen(ScreenType::MixMaster);
}

void AppController::showServiceGaveUpNotice(bool portTaken)
{
    // DSK-17: name the real cause when another program
    // owns the service port - "check Python" sent users
    // after the wrong problem.
    const juce::String cause =
        portTaken ? TRANS("Port") + " " + juce::String(PythonServiceManager::kPort) + " " +
                        TRANS("is in use by another process, so the analysis service cannot "
                              "start. Close the program using that port, then restart the "
                              "service.")
                  : TRANS("Check that Python 3 is installed and local-services dependencies "
                          "are set up, then try restarting it.");
    showServiceError(TRANS("Local analysis service failed to start after several attempts.") + " " + cause);
    // A passive banner alone left the user with no way to act on "it gave
    // up" short of knowing to open the command palette (audit ARCH-06):
    // offer the same restart path that command does. A notice, not a modal
    // box - the decision is single-action and non-destructive - posted under
    // the engine-state id, so the next status change withdraws it.
    juce::Component::SafePointer<AppController> self(this);
    decision_notices::postReplacing(noticeBar_, decision_notices::serviceGaveUp(cause, [self] {
                                        if (self == nullptr) return;
                                        self->lifecycle_.resetGaveUpNotice();
                                        self->lifecycle_.restartPythonService(); // off the message thread; resets the backoff
                                    }));
}

// ---- Results gate ----------------------------------------------------------

bool AppController::screenShowsResults(ScreenType s) const noexcept
{
    switch (s) {
    case ScreenType::Dashboard:
    case ScreenType::AiDetection:
    case ScreenType::VocalWorkspace:
    case ScreenType::MixMaster:
    case ScreenType::SystemBrain:
    case ScreenType::Export:
    case ScreenType::Copilot:
    case ScreenType::Issues:
        return true;
    case ScreenType::Intake:
    case ScreenType::ReferenceLibrary:
    case ScreenType::Settings:
        return false;
    }
    return false;
}

void AppController::applyResultsGate(AnalysisService::ResultsGate gate)
{
    using ResultsGate = AnalysisService::ResultsGate;
    switch (gate) {
    case ResultsGate::Analyzing:
        analysisState_ = SessionStateStrip::State::Analysis::Analysing;
        refreshSessionStateStrip();
        resultsOverlay_.setMode(AnalysisPendingOverlay::Mode::Analyzing);
        break;
    case ResultsGate::Cancelled:
        analysisState_ = hasAnalysis() ? SessionStateStrip::State::Analysis::Ready : SessionStateStrip::State::Analysis::None;
        refreshSessionStateStrip();
        resultsOverlay_.setMode(AnalysisPendingOverlay::Mode::Cancelled);
        break;
    case ResultsGate::Open:
        if (hasAnalysis()) {
            analysisState_ = SessionStateStrip::State::Analysis::Ready;
            refreshSessionStateStrip();
            updateSessionStatusLabel(); // "Ready"
        }
        if (stemsAutoStartPending_) {
            stemsAutoStartPending_ = false;
            autoStartStemSeparation();
        }
        break;
    case ResultsGate::Failed: // the failure text comes from onFailed / the caller
        break;
    }
    updateResultsOverlay();
}

void AppController::updateResultsOverlay()
{
    const bool show = analysis_.resultsGate() != AnalysisService::ResultsGate::Open && hasActiveSession() &&
                      screenShowsResults(currentScreen);
    if (show) {
        resultsOverlay_.setBounds(contentContainer.getBounds());
        resultsOverlay_.toFront(false);
        serviceErrorBanner_.toFront(false);
    }
    if (show != resultsOverlay_.isVisible()) resultsOverlay_.setVisible(show);
}

} // namespace khensu
