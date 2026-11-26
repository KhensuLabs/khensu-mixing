// desktop-app/Source/AppController_Guardrail.cpp
// The post-render guardrail prompt: a render the service refused, with the
// option to override it with a note.
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
#include "ui/ProvenanceSource/AiDetectionScreen.h"
#include "midi/Midi2DeviceManager.h"
#include "ui/shared/RenderPaths.h"
#include "CrashReporter.h"
#include "ui/shared/JobPoller.h"
#include <khensu/ipc/generated/ApiSchema.h>

namespace apif = khensu::api::fields;

namespace khensu {

void AppController::showGuardrailOverridePrompt(const GuardrailRejection& rejection)
{
    juce::String message = "The rendered output failed the post-render safety guardrail (" +
                           rejection.guardrailStatus + ") and was quarantined instead of saved to the "
                           "requested location.\n\n";
    if (!rejection.measurements.isVoid())
        message << "Measurements:\n" << juce::JSON::toString(rejection.measurements, true) << "\n\n";
    message << "Type an explicit reason below to promote the quarantined render anyway (recorded in this "
               "session's provenance timeline), or Discard to leave it quarantined.";

    // Owned by JUCE's modal manager (deleteWhenDismissed=true below) - the
    // callback below reads its text editor before that deletion happens.
    auto* aw = new juce::AlertWindow("Render Rejected By Safety Guardrail", message, juce::AlertWindow::WarningIcon);
    aw->addTextEditor("expertNote", {}, "Reason for overriding (required)");
    aw->addButton("Override", 1);
    aw->addButton("Discard", 0);

    juce::Component::SafePointer<AppController> self(this);
    auto callback = juce::ModalCallbackFunction::create([self, aw, rejection](int result) {
        const juce::String note = aw->getTextEditorContents("expertNote");
        if (result != 1 || self == nullptr) return; // Discard, or window closed
        if (note.trim().isEmpty()) {
            self->showServiceError("An override reason is required - the render remains quarantined.");
            return;
        }
        self->requestGuardrailOverride(rejection, note);
    });
    aw->enterModalState(true, callback, true);
}

void AppController::requestGuardrailOverride(const GuardrailRejection& rejection, const juce::String& expertNote)
{
    juce::Component::SafePointer<AppController> self(this);
    runInBackground([self, rejection, expertNote]() {
        ipc::HttpServiceBridge bridge;
        auto result = bridge.overrideGuardrail(rejection.sourcePath, rejection.quarantinePath, rejection.outputPath,
                                               expertNote, rejection.sessionId, rejection.sourceRecommendationId,
                                               rejection.decisionHash);
        juce::MessageManager::callAsync([self, result = std::move(result)]() mutable {
            if (self == nullptr) return;
            if (result.is_ok())
                self->showExplainer(TRANS("Guardrail override"), TRANS("Diagnose"),
                                    TRANS("The render was promoted despite the guardrail rejection. This has been "
                                    "recorded in the session's provenance timeline."));
            else
                self->showServiceError("Override failed: " + juce::String(result.error().message));
        });
    });
}

} // namespace khensu
