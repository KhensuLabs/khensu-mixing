#pragma once
#include <JuceHeader.h>
#include <functional>
#include "KhensuTheme.h"
#include "../../IAppController.h"
#include <khensu/ipc/HttpServiceBridge.h>

namespace khensu {

class ModelDownloadDialog : public juce::Component, public juce::Timer {
  public:
    explicit ModelDownloadDialog(IAppController& c);
    ~ModelDownloadDialog() override;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void timerCallback() override;

  private:
    // All bridge traffic runs off the message thread (synchronous-bridge-call
    // sweep): runBridgeAction launches a worker that performs an optional
    // mutation and always re-reads /models/status, marshalling the result back
    // to applyStatus via MessageManager::callAsync. The Timer only triggers a
    // guarded refresh; it never touches the bridge on the message thread.
    using BridgeAction = std::function<khensu::Result<void, ipc::BridgeError>(ipc::HttpServiceBridge&)>;
    void runBridgeAction(BridgeAction action);
    void refreshStatus();
    void applyStatus(const ipc::HttpServiceBridge::ModelStatus& ms);
    void selectCustomFolder();

    IAppController& controller_;

    juce::Label titleLabel_;
    juce::Label descLabel_;
    juce::Label folderLabel_;
    juce::TextButton changeFolderButton_;
    juce::TextButton actionButton_; // Start / Pause / Resume
    juce::TextButton cancelButton_; // Cancel / Close

    float progress_ = 0.0f;
    juce::String statusText_;
    juce::String folderPath_;
    juce::String statusState_;
    bool installed_ = false;
    bool statusRequestInFlight_ = false;
    juce::String actionError_; ///< last failed mutation (ServiceErrorHint line); cleared by the next action
    /// Re-applies every cached palette colour. Called at construction and on
    /// each theme change.
    void applyThemeColours();

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ModelDownloadDialog)
};

} // namespace khensu
