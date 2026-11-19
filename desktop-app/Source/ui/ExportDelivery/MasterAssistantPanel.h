// desktop-app/Source/ui/ExportDelivery/MasterAssistantPanel.h
// FEAT-2 / WF-3: the Master Assistant - mastering on one card, from the
// session's own target. 1 Master: the mastering agent runs on the current
// version toward the session's genre; its candidate goes straight into the
// loudness-matched A/B. 2 Shape: five macros (loudness, tone, width, punch,
// ceiling) re-render the agent's chain as a new candidate. 3 Accept: the
// candidate becomes the current version, and the fix check verifies it
// against the original's findings. 4 Export: the platform masters.
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include "MasterMacros.h"
#include "../shared/KhensuTheme.h" // ThemeListener
#include "../shared/ListenerHandle.h"
#include "../shared/PanelCard.h"
#include <array>
#include <atomic>
#include <functional>
#include <memory>

namespace khensu {

class IAppController;

class MasterAssistantPanel final : public PanelCard {
  public:
    explicit MasterAssistantPanel(IAppController& c);
    ~MasterAssistantPanel() override;
    void resized() override;
    void resetForSession() override;
    void visibilityChanged() override;

    /// Step 4: export the accepted master for the platforms (the screen
    /// runs its Platform Masters set).
    std::function<void()> onExportPlatforms;

    /// The macros as set on the sliders (tests read them).
    master_macros::Macros macros() const;
    /// The agent chain the macros shape (empty until a run finished).
    const master_macros::Chain& baseChain() const noexcept { return baseChain_; }

    /// Re-reads the session's target (the step calls it when shown).
    void refreshTarget();

  private:
    void master();
    void cancel();
    void poll(const juce::String& jobId);
    void onFinished(const ipc::HttpServiceBridge::MasteringJobStatus& status);
    void shape();
    void accept();
    void showVerification();
    void setStage(int stage); ///< 0 idle, 1 mastering, 2 candidate ready, 3 accepted
    bool sessionChanged() const;

    juce::Label targetLabel_;
    juce::TextButton masterButton_{TRANS("Master it")};
    juce::TextButton cancelButton_{TRANS("Cancel")};
    juce::Label statusLabel_;

    struct Macro {
        juce::Label label;
        juce::Slider slider{juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight};
    };
    std::array<Macro, 5> macros_; ///< loudness, tone, width, punch, ceiling
    juce::TextButton shapeButton_{TRANS("Render with these macros")};
    juce::TextButton resetMacrosButton_{TRANS("Reset")};
    juce::TextButton acceptButton_{TRANS("Accept this master")};
    juce::Label verifyLabel_;
    juce::TextButton exportButton_{TRANS("Export for platforms...")};

    juce::String jobId_;
    juce::String jobSessionId_;
    juce::String inputPath_;     ///< what the agent mastered (the pre-master version)
    juce::String candidatePath_; ///< the candidate in the A/B
    juce::String acceptedPath_;  ///< the accepted master, for the check
    juce::String decisionHash_;
    master_macros::Chain baseChain_;
    std::shared_ptr<std::atomic<bool>> pollStop_;
    int stage_ = 0;

    void applyThemeColours();

    // After everything the callback touches.
    ListenerHandle verificationListener_;
    ListenerHandle analysisListener_;

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MasterAssistantPanel)
};

} // namespace khensu
