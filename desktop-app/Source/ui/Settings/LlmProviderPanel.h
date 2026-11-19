// desktop-app/Source/ui/Settings/LlmProviderPanel.h
#pragma once
#include <JuceHeader.h>
#include "../shared/ExperimentalBadge.h"
#include "../shared/KhensuTheme.h" // ThemeListener
#include "../../IAppController.h"
#include <khensu/ipc/HttpServiceBridge.h>
#include <memory>
#include <string>

namespace khensu {

/// Local-LLM part of the Settings provider picker (CE-AG-2): what
/// /copilot/providers detected on localhost (llama.cpp server, Ollama,
/// LM Studio), whether an in-process GGUF model can run, which provider is
/// actually in use, and the two local settings (GGUF model path, an extra
/// loopback server URL). Local providers are never blocked by the online
/// master switch; cloud providers still are.
// THREAD_CONTEXT(UIThread)
class LlmProviderPanel final : public juce::Component {
  public:
    /// `configDir` holds settings.json; empty means the app data folder.
    explicit LlmProviderPanel(IAppController& c, std::string baseUrl = ipc::HttpServiceBridge::kDefaultBaseUrl,
                              juce::File configDir = {});
    ~LlmProviderPanel() override = default;

    void resized() override;
    /// Preferred height for the parent's layout.
    static constexpr int kPreferredHeight = 150;

    /// Re-query the service (refresh=true re-probes localhost).
    void refresh(bool reprobe);

    /// One readable block for a ProvidersResponse JSON. Static + public so
    /// it is unit-testable without a service.
    static juce::String formatReport(const juce::var& report);

  private:
    void saveLocalSettings();

    IAppController& controller_;
    std::string baseUrl_;
    juce::File configDir_;
    juce::Label headingLabel_{{}, TRANS("Local LLM (no internet needed)")};
    ExperimentalBadge experimentalBadge_; ///< VER-8
    juce::Label reportLabel_;
    juce::TextButton detectButton_{TRANS("Detect local models")};
    juce::Label ggufLabel_{{}, TRANS("GGUF model file")};
    juce::TextEditor ggufField_;
    juce::TextButton browseButton_{TRANS("Browse...")};
    juce::Label urlLabel_{{}, TRANS("Other local server URL")};
    juce::TextEditor urlField_;
    std::unique_ptr<juce::FileChooser> chooser_;

    void applyThemeColours();
    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LlmProviderPanel)
};

} // namespace khensu
