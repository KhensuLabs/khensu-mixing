// desktop-app/Source/ui/ExportDelivery/BatchRenderPanel.h
// Batch Render: apply the session's last-applied effect chain to every audio
// file in a folder through the service's /copilot/apply_chain/batch job queue.
// Per-file success/failure - one corrupt take doesn't sink the batch.
// THREAD_CONTEXT(UIThread)
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
#include <khensu/ipc/HttpServiceBridge.h>

namespace khensu {

class BatchRenderPanel final : public PanelCard {
  public:
    explicit BatchRenderPanel(PanelCardController& c, const juce::String& baseUrl = {});
    ~BatchRenderPanel() override;
    void resized() override;
    void visibilityChanged() override;

    /// Fired when the user asks to override a guardrail-REJECTED output (the
    /// file sits at its quarantine path). ExportScreen wires it to
    /// AppController::showGuardrailOverridePrompt - the expert-justification
    /// flow every other render uses.
    std::function<void(const GuardrailRejection&)> onGuardrailOverride;

  public: // Action seam for the fake-service regression tests.
    void chooseFolder();
    void run();
    void cancelRun();
    /// Waits for `jobId` on the long pool through the shared JobPoller
    /// (bounded consecutive errors, 404 terminal - UIX-07).
    void pollJob(const juce::String& jobId);
    /// Re-checks folder + chain availability and updates the run button/hint.
    void refreshReadiness();

#if KHENSU_TESTS
    void setInputFolderForTesting(const juce::File& folder);
    const juce::String statusTextForTesting() const { return statusLabel_.getText(); }
#endif

  private:

    juce::TextButton chooseButton_{juce::String::fromUTF8("Choose Folder...")};
    juce::TextButton runButton_{TRANS("Process Folder")};
    juce::TextButton cancelButton_{TRANS("Cancel")};
    juce::Label statusLabel_;
    juce::Label resultsLabel_;
    /// Guardrail-rejected outputs of the last run, offered for override.
    std::vector<GuardrailRejection> rejected_;
    juce::TextButton overrideButton_{TRANS("Override...")};
    void offerOverride();

    juce::File folder_;
    int audioFileCount_ = 0;
    juce::String currentJobId_;
    bool startInFlight_ = false; // /start call in flight, no jobId yet
    bool cancelRequestedWhileStarting_ = false;
    std::shared_ptr<std::atomic<bool>> pollStop_;
    std::string bridgeUrl() const; // the injected base URL (test seam), else the default
    void finishRun(const ipc::HttpServiceBridge::BatchRenderJobStatus& s);
    std::unique_ptr<juce::FileChooser> chooser_;
    juce::String baseUrl_;
    /// Re-applies every cached palette colour. Called at construction and on
    /// each theme change.
    void applyThemeColours();

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BatchRenderPanel)
};

} // namespace khensu
