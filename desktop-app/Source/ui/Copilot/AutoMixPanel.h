// desktop-app/Source/ui/Copilot/AutoMixPanel.h
#pragma once
#include <JuceHeader.h>
#include "../shared/KhensuTheme.h" // ThemeListener
#include "../../IAppController.h"
#include <khensu/ipc/HttpServiceBridge.h>
#include <atomic>
#include <memory>
#include <string>

namespace khensu {

/// "Auto-mix (verified)" (CE-AG-1, backed by /agent/mix): the agent plans
/// steps from the findings, renders each one, re-measures at matched loudness
/// and keeps a step only when it improved its target without making anything
/// else worse and passed the guardrail. The step log updates live while the
/// job runs; the finished result loads into the A/B "B" slot for audition and
/// can then be accepted into the render history (each kept step as a node).
// THREAD_CONTEXT(UIThread)
class AutoMixPanel final : public juce::Component {
  public:
    explicit AutoMixPanel(IAppController& c, std::string baseUrl = ipc::HttpServiceBridge::kDefaultBaseUrl);
    ~AutoMixPanel() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

    // Test entry points (the buttons call these).
    void run();
    void cancelRun();
    void auditionResult();
    void acceptResult();
    juce::String statusText() const { return statusLabel_.getText(); }
    juce::String logText() const { return log_.getText(); }
    bool isRunning() const noexcept { return currentJobId_.isNotEmpty(); }

    /// Renders a job status payload (AgentMixStatusResponse JSON) as the
    /// step log. Static + public so it is unit-testable without a service.
    static juce::String formatStepLog(const juce::var& status);

  private:
    void pollJob(const juce::String& jobId);
    void onProgress(const juce::var& status);
    void onFinished(const juce::var& status);
    void setRunning(bool running);
    juce::var findingsFromAnalysis() const;

    IAppController& controller_;
    std::string baseUrl_;

    juce::Label targetLabel_{{}, TRANS("Target loudness (LUFS)")};
    juce::Slider targetSlider_;
    juce::Label stepsLabel_{{}, TRANS("Max steps")};
    juce::Slider stepsSlider_;
    juce::ToggleButton useRecsToggle_{TRANS("Act on this session's recommendations")};
    juce::ToggleButton loudnessToggle_{TRANS("Include loudness / true peak")};
    juce::TextButton runButton_{TRANS("Run verified auto-mix")};
    juce::TextButton cancelButton_{TRANS("Cancel")};
    juce::TextButton auditionButton_{TRANS("Audition result (A/B)")};
    juce::TextButton acceptButton_{TRANS("Accept into render history")};
    juce::Label statusLabel_;
    juce::TextEditor log_;

    juce::String currentJobId_;
    juce::String jobSessionId_;
    juce::String jobInputPath_; ///< WF-1: what the run processed
    juce::var lastResult_; ///< AgentMixResult of the finished run
    std::shared_ptr<std::atomic<bool>> pollStop_;

    void applyThemeColours();
    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AutoMixPanel)
};

} // namespace khensu
