// desktop-app/Source/ui/ExportDelivery/PlatformMastersPanel.h
// One-click platform-target master render set: renders the session's master to
// each streaming/club target loudness (Spotify -14 / Apple -16 / club -9 /
// YouTube), true-peak safe and through the post-render guardrail, in one action
// via /copilot/platform_masters. THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include "../shared/KhensuTheme.h" // ThemeListener
#include "../shared/PanelCard.h"
#include "../../RenderJobManager.h" // GuardrailRejection
#include <functional>
#include <vector>
#include <atomic>
#include <memory>
#include <string>

namespace khensu {

class PlatformMastersPanel final : public PanelCard {
  public:
    explicit PlatformMastersPanel(PanelCardController& c, const juce::String& baseUrl = {});
    ~PlatformMastersPanel() override;
    void resized() override;
    void visibilityChanged() override;

    /// Fired when the user asks to override a guardrail-REJECTED output (the
    /// file sits at its quarantine path). ExportScreen wires it to
    /// AppController::showGuardrailOverridePrompt - the expert-justification
    /// flow every other render uses.
    std::function<void(const GuardrailRejection&)> onGuardrailOverride;

    /// Re-reads whether a session is loaded, unless a job is running. The
    /// screen calls it when it shows this panel's step: JUCE sends
    /// visibilityChanged() to the step page, not to the panels on it, so
    /// the panel kept its launch-time "Load a session first." (2026-11-18).
    void refreshIfIdle();

  public: // Action seam for the fake-service regression tests.
    void run();
    void cancelRun();
    /// Waits for `jobId` on the long pool through the shared JobPoller
    /// (bounded consecutive errors, 404 terminal - UIX-07).
    void pollJob(const juce::String& jobId);
    void refreshReadiness();

#if KHENSU_TESTS
    const juce::String statusTextForTesting() const { return statusLabel_.getText(); }
#endif

  private:

    juce::TextButton runButton_{TRANS("Render safety variants")};
    /// CUT-9: off = each version keeps the master's loudness (safety variant);
    /// on = each is brought to its platform's loudness target as well.
    juce::ToggleButton matchLoudnessToggle_{TRANS("Match each platform's loudness")};
    juce::TextButton cancelButton_{TRANS("Cancel")};
    juce::Label statusLabel_;
    juce::Label resultsLabel_;
    /// Guardrail-rejected outputs of the last run, offered for override.
    std::vector<GuardrailRejection> rejected_;
    juce::String runSourcePath_; ///< the master the last run rendered from
    juce::TextButton overrideButton_{TRANS("Override...")};
    void offerOverride();

    juce::String currentJobId_;
    bool startInFlight_ = false; // /start call in flight, no jobId yet
    bool cancelRequestedWhileStarting_ = false;
    std::shared_ptr<std::atomic<bool>> pollStop_;
    juce::String baseUrl_; // empty = the default service address
    /// A bridge at the injected base URL (the test seam), else the default.
    std::string bridgeUrl() const;
    void finishRun(const juce::String& status, const juce::var& results);
    /// Re-applies every cached palette colour. Called at construction and on
    /// each theme change.
    void applyThemeColours();

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PlatformMastersPanel)
};

} // namespace khensu
