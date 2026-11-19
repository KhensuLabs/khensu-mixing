// desktop-app/Source/AppController_Playback.cpp
// What is left of playback in the controller: the ABX overlay, the reference
// track, learned taste and the plugin link. Playback itself is PlaybackService
// (ENG-1 stage C).
// Split out of AppController.cpp; see AppController.h for the declarations.
#include "AppController.h"
#include "ui/shared/AbxTrialOverlay.h"
#include "ui/shared/ChainReasoningOverlay.h"
#include "ui/shared/ExplainerOverlay.h"
#include "ui/shared/MetricDiffOverlay.h"
#include "ui/shared/OnboardingWizard.h"
#include "ui/shared/ReferenceTrackRitualOverlay.h"
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
#include <khensu/feedback/LearnedPreferences.h>
#include <khensu/ipc/HttpServiceBridge.h>
#include <khensu/analysis/AudioAnalyzer.h>
#include <khensu/analysis/LoudnessAnalyzer.h>
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
#include <khensu/session/AudioFileProber.h>

namespace khensu {


void AppController::showAbxTrial(const juce::StringArray& blindReferencePaths)
{
    // DSK-21: replacing an open overlay must restore what it borrowed first.
    hideAbxTrial();
    activeAbxOverlay_ = std::make_unique<AbxTrialOverlay>(*this, blindReferencePaths);
    activeAbxOverlay_->onDismiss = [this]() { hideAbxTrial(); };
    addAndMakeVisible(activeAbxOverlay_.get());
    activeAbxOverlay_->setBounds(getLocalBounds());
}


void AppController::hideAbxTrial()
{
    if (activeAbxOverlay_ != nullptr) {
        // Put back the B side (and loaded side) the trial borrowed: a blind
        // reference left in the B slot became the next export (AUDIT-001).
        activeAbxOverlay_->restorePlaybackState();
        removeChildComponent(activeAbxOverlay_.get());
        activeAbxOverlay_.reset();
    }
}


} // namespace khensu
