// desktop-app/Source/AppController_Diagnostics.cpp
// The diagnostics host (IDiagnosticsHost): what --screenshot, --ui-bench and
// --screenshot-audio drive - the screen list, a throwaway session, the
// results-gate snapshot, an Intake drop and fixing the top issue.
// Split out of AppController.cpp (REF-8).
#include "AppController.h"
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
#include "ui/ReferenceLibrary/ReferenceLibraryScreen.h"
#include "session/WorkspaceRecoveryState.h"
#include "ui/Settings/SettingsScreen.h"
#include "ui/AiDetection/AiDetectionScreen.h"
#include "midi/Midi2DeviceManager.h"
#include "ui/shared/RenderPaths.h"
#include "CrashReporter.h"
#include "ui/shared/JobPoller.h"
#include <khensu/ipc/generated/ApiSchema.h>

namespace apif = khensu::api::fields;

namespace khensu {

std::vector<IDiagnosticsHost::Screen> AppController::diagnosticScreens()
{
    return {{"Intake", "01-intake", ScreenType::Intake, intakeScreen.get()},
            {"Dashboard", "02-dashboard", ScreenType::Dashboard, dashboardScreen.get()},
            {"AI Detection", "03-ai-detection", ScreenType::AiDetection, ensureScreenBuilt(ScreenType::AiDetection)},
            {"Vocal", "04-vocal", ScreenType::VocalWorkspace, vocalWorkspaceScreen.get()},
            {"Mix / Master", "05-mix-master", ScreenType::MixMaster, mixMasterScreen.get()},
            {"Reference Library", "06-reference-library", ScreenType::ReferenceLibrary, ensureScreenBuilt(ScreenType::ReferenceLibrary)},
            {"System Brain", "07-system-brain", ScreenType::SystemBrain, systemBrainPanel.get(), true},
            {"Export", "08-export", ScreenType::Export, exportScreen.get()},
            {"Copilot", "09-copilot", ScreenType::Copilot, ensureScreenBuilt(ScreenType::Copilot)},
            {"Issues", "10-issues", ScreenType::Issues, issuesScreen.get()},
            {"Settings", "11-settings", ScreenType::Settings, settingsScreen.get()}};
}

void AppController::publishAnalysisForDiagnostics(const AnalysisResult& result)
{
    refreshSessionStateStrip();
    analysis_.analysisModel().setResult(result);
}

bool AppController::startDiagnosticSession(const juce::File& audio)
{
    AudioFileRef ref;
    ref.id = juce::Uuid().toString().toStdString();
    ref.path = audio.getFullPathName().toStdString();
    ref.format = audio.getFileExtension().trimCharactersAtStart(".").toLowerCase().toStdString();
    suppressCoach_ = true; // a throwaway session, not the user's first (UX-15)
    CreativeTarget target;
    // VIZ-F3: the demo song is synthpop - with its genre the screenshots
    // show the genre window and the released-track spread.
    if (audio.getFileName().startsWithIgnoreCase("demo-song") || audio.getFileName().startsWithIgnoreCase("Khensu Demo")) {
        target.primary_genre = "synthpop";
        target.playback_intent = "streaming";
    }
    return startSession("screenshot check", {ref}, target);
}

void AppController::deleteDiagnosticSession(std::function<void()> done)
{
    deleteActiveSessionAndDerivedData([done = std::move(done)](bool, juce::String) {
        if (done) done();
    });
}

IDiagnosticsHost::GateSnapshot AppController::gateSnapshot() const
{
    GateSnapshot g;
    const auto gate = analysis_.resultsGate();
    g.pending = analysis_.resultsPending();
    g.overlayCovers = resultsOverlay_.isVisible() && resultsOverlay_.getBounds() == contentContainer.getBounds();
    g.gate = static_cast<int>(gate);
    g.open = gate == AnalysisService::ResultsGate::Open;
    g.ended = gate == AnalysisService::ResultsGate::Failed || gate == AnalysisService::ResultsGate::Cancelled;
    g.pendingScans = analysis_.followUpPending();
    g.inFlight = analysis_.analysisInFlight();
    g.hasResult = hasAnalysis();
    g.deep = g.hasResult && currentAnalysis().has_python_analysis();
    g.stemsRunning = stems_.isSeparatingStems() || stemsAutoStartPending_;
    g.stemsAvailable = stems_.hasStemsAvailable();
    g.stemMessage = stems_.lastAutoStemMessage();
    g.currentRender = renders_.currentRenderPath();
    g.verifying = analysis_.verificationRunning();
    if (const auto* v = analysis_.currentVerification()) g.verification = juce::String(fix_verification::summary(*v));
    return g;
}

void AppController::dropIntoIntakeForDiagnostics(const juce::File& audio)
{
    if (intakeScreen) intakeScreen->dropFilesForDiagnostics({audio});
}

bool AppController::intakeReadyForDiagnostics() const
{
    return intakeScreen != nullptr && intakeScreen->createSessionReady();
}

std::vector<std::pair<juce::String, juce::Component*>> AppController::dashboardCardsForDiagnostics()
{
    return dashboardScreen != nullptr ? dashboardScreen->groupCardsForDiagnostics()
                                      : std::vector<std::pair<juce::String, juce::Component*>>{};
}

int AppController::showExportStepForDiagnostics(int step)
{
    showScreen(ScreenType::Export);
    if (exportScreen == nullptr) return 0;
    exportScreen->showStep(step);
    return ExportScreen::kStepCount;
}

int AppController::showMixMasterViewForDiagnostics(int view)
{
    showScreen(ScreenType::MixMaster);
    if (mixMasterScreen == nullptr) return 0;
    mixMasterScreen->showView(view);
    return MixMasterScreen::kViewCount;
}

void AppController::playForDiagnostics(double fromSec, bool play)
{
    if (!play) {
        playback().stopAudio();
        return;
    }
    playback().setPlaybackPosition(fromSec);
    playback().playAudio();
}

void AppController::fixTopIssueForDiagnostics(std::function<void(bool, juce::String)> done)
{
    if (!issues_.fixTopIssue(done) && done) done(false, "no open issue has a one-click fix");
}

} // namespace khensu
