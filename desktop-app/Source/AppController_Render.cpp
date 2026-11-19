// desktop-app/Source/AppController_Render.cpp
// Automatic stem separation (audit refactor 'Split AppController'). The render history, renders, delta solo, export and reports moved to session/RenderService (ENG-1 stage C2); stem separation itself to session/StemService (ENG-1 stage C3). The results-gate trigger stays here.
// Split out of AppController.cpp; see AppController.h for the declarations.
#include "session/MultitrackStemBuses.h"
#include "AppController.h"
#include "ui/shared/AbxTrialOverlay.h"
#include "ui/shared/ChainReasoningOverlay.h"
#include "ui/shared/ExplainerOverlay.h"
#include "ui/shared/MetricDiffOverlay.h"
#include "ui/shared/OnboardingWizard.h"
#include "ui/shared/ReferenceTrackRitualOverlay.h"
#include <khensu/ipc/generated/ApiJobStatus.h>
#include "ui/shared/BackgroundStop.h"
#include "midi/MacroControlRouter.h"
#include "ui/Settings/MacroMappingPanel.h"
#include "ui/shared/AccessibilityCoverage.h"
#include "BackgroundReadKey.h"
#include "ui/shared/KhensuTheme.h"
#include "ui/shared/KhensuLookAndFeel.h"
#include "ui/shared/CloudChatConsentController.h"
#include <cmath>
#include <thread>
#include <khensu/common/AppPaths.h>
#include <khensu/common/FileAcl.h>
#include <khensu/session/OutboxDispatcher.h>
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
#include "ui/shared/ServiceErrorHint.h"
#include <khensu/ipc/generated/ApiSchema.h>

// Service JSON field names, generated from resources/api/openapi.json:
// a renamed Python field is a compile error here, not a silent 0.
namespace apif = khensu::api::fields;

#include "ui/shared/JobPoller.h"

namespace khensu {

void AppController::autoStartStemSeparation()
{
    if (!sessions_.state().session || sessions_.state().session->files.empty()) return;
    // Any stem files - real or the approximate HPSS split - mean a separation
    // already ran for this source; re-running it on every open only burns
    // minutes of CPU. A manual "Isolate Stems" can still retry Demucs.
    if (stems_.stemFilesExist()) return;
    // A multitrack session already has the real stems; guessing them back out
    // of their own mixdown would only cost minutes of Demucs. Its tracks are
    // summed into the four stem slots instead, so the stem tools see them
    // (WF-7).
    if (sessions_.state().session->is_multitrack()) {
        const auto files = sessions_.state().session->files;
        const auto dir = stems_.getStemsDirectory();
        const auto sessionId = sessions_.state().session->id;
        juce::Component::SafePointer<AppController> self(this);
        runInBackground([self, files, dir, sessionId] {
            const auto outcome = multitrack_stems::writeStemBuses(files, dir);
            juce::MessageManager::callAsync([self, outcome, sessionId] {
                if (self == nullptr) return;
                auto& state = self->sessions_.state();
                if (!state.session || state.session->id != sessionId) return; // superseded session
                self->stems_.setLastAutoStemMessage((outcome.ok ? "ok: " : "failed: ") + outcome.message);
                juce::Logger::writeToLog("Multitrack stems " + self->stems_.lastAutoStemMessage());
                if (!outcome.ok) return;
                if (self->stems_.onStemProgress) self->stems_.onStemProgress("stems:completed:1.0", 6, 6);
                self->stems_.ensureVocalAnalysis();
            });
        });
        return;
    }

    stems_.setLastAutoStemMessage({});
    stems_.runStemSeparation("demucs", [this](bool success, juce::String message) {
        // No dialog - this is a background convenience triggered at intake,
        // not a user action - but the outcome is kept (lastAutoStemMessage())
        // so a failure is explainable instead of silently leaving no stems.
        stems_.setLastAutoStemMessage((success ? "ok: " : "failed: ") + message);
        juce::Logger::writeToLog("Auto stem separation " + stems_.lastAutoStemMessage());
    });
}

} // namespace khensu
