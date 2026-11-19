// desktop-app/Source/AppController_Onboarding.cpp
// First-run onboarding: the wizard and applying the plan it returns.
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

void AppController::showOnboardingWizard()
{
    onboardingWizard_ = std::make_unique<OnboardingWizard>(analysis_.genreDatabase());
    juce::Component::SafePointer<AppController> safe(this);
    onboardingWizard_->onFinish = [safe](const OnboardingPlan& plan, bool push) {
        // Deferred: the wizard is still inside its own button callback.
        juce::MessageManager::callAsync([safe, plan, push] {
            if (auto* self = safe.getComponent()) self->finishOnboarding(plan, push);
        });
    };
    onboardingWizard_->onSkip = [safe] {
        juce::MessageManager::callAsync([safe] {
            auto* self = safe.getComponent();
            if (self == nullptr) return;
            self->onboardingWizard_.reset();
            self->sessionManager().setUserPreference("onboarding_profile", R"({"skipped": true})");
        });
    };
    addAndMakeVisible(onboardingWizard_.get());
    onboardingWizard_->setBounds(getLocalBounds());
    onboardingWizard_->toFront(true);
}

void AppController::finishOnboarding(const OnboardingPlan& plan, bool pushToPlugin)
{
    onboardingWizard_.reset();
    auto* o = new juce::DynamicObject();
    o->setProperty("goal", OnboardingPlan::goalName(plan.goal));
    o->setProperty("genre_id", plan.genreId);
    o->setProperty("platform", OnboardingPlan::platformName(plan.platform));
    o->setProperty("target_lufs", plan.targetLufs);
    o->setProperty("true_peak_ceiling", plan.truePeakCeiling);
    sessionManager().setUserPreference("onboarding_profile", juce::JSON::toString(juce::var(o), true).toStdString());

    const auto params = plan.combinedParams();
    // WF-9: through the one plugin-chain model; ticking "send to the plugin"
    // in setup is the user's go-ahead, so it applies at once.
    if (pushToPlugin && !params.empty()) {
        lifecycle_.stagePluginChain(params, TRANS("Onboarding"));
        lifecycle_.applyStagedPluginChain();
    }

    // P0-4: "Match a reference" used to open the Reference Library, which
    // can't load a reference. Intake can: its "Match reference track" source
    // takes the file, and the session it starts loads it for comparison.
    showScreen(ScreenType::Intake);
    if (intakeScreen != nullptr) {
        intakeScreen->presetGenre(plan.genreId);
        if (plan.goal == OnboardingPlan::Goal::MatchReference) intakeScreen->startMatchReference();
    }
    juce::String next = plan.summary() + "\n\n";
    next << (plan.goal == OnboardingPlan::Goal::MatchReference
                 ? TRANS("Choose your reference track (Match reference track, on Intake), then drop your mix. The "
                         "session compares against it from the start.")
                 : TRANS("Drop your audio here to analyze it against these targets."));
    if (pushToPlugin && !params.empty()) next << " The starting chain was sent to the Copilot plugin.";
    next << "\n\nReopen setup anytime: Ctrl+K, \"onboarding\".";
    showExplainer(TRANS("You're set up"), TRANS("Onboarding"), next);
}

} // namespace khensu
