#pragma once
#include <functional>
#include <JuceHeader.h>
#include <khensu/ipc/HttpServiceBridge.h>

namespace khensu {

class KhensuCopilotProcessor; // forward

class MixerChainMiniPanel final : public juce::Component {
  public:
    explicit MixerChainMiniPanel(KhensuCopilotProcessor* proc = nullptr);
    ~MixerChainMiniPanel() override = default;

    void paint(juce::Graphics& g) override;
    void resized() override;

    /// WF-9: after the chain is loaded into the DSP chain, the editor shows
    /// it there - the plugin chain's one editor.
    std::function<void()> onLoadedIntoDsp;

  private:
    void generateChain();
    void updateResultsView();
    void applyChainToProcessor();

    // Inputs (Left Column)
    juce::Label targetLabel;
    juce::ComboBox targetCombo;

    juce::Label genreLabel;
    juce::ComboBox genreCombo;

    juce::Label energyLabel;
    juce::ComboBox energyCombo;

    juce::Label moodLabel;
    juce::ComboBox moodCombo;

    juce::Label bpmLabel;
    juce::TextEditor bpmInput;

    juce::Label styleLabel;
    juce::TextEditor styleInput;

    // Ecosystem Toggles
    juce::Viewport ecoViewport;
    juce::Component ecoContent;
    std::vector<std::unique_ptr<juce::ToggleButton>> ecoToggles;

    juce::TextButton generateButton;
    juce::TextButton applyButton;

    // Results (Right Column)
    juce::Label descriptionLabel;
    juce::Viewport resultsViewport;
    juce::Component resultsContent;
    juce::OwnedArray<juce::Component> resultsComponents;

    bool isGenerating_ = false;
    ipc::HttpServiceBridge::MixerChainResponse lastResponse_;
    KhensuCopilotProcessor* processor_ = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MixerChainMiniPanel)
};

} // namespace khensu
