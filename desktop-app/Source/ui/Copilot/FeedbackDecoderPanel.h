// desktop-app/Source/ui/Copilot/FeedbackDecoderPanel.h
// Client Revision Decoder: paste what the client actually said ("make it
// warmer", "the vocals feel buried") and get ranked engineering
// interpretations, each with concrete parameters and a one-click apply
// that renders through the same pipeline as every other recommendation.
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include "../shared/KhensuTheme.h" // ThemeListener
#include <vector>

namespace khensu {

class IAppController;

class FeedbackDecoderPanel final : public juce::Component {
  public:
    explicit FeedbackDecoderPanel(IAppController& c);

    void paint(juce::Graphics& g) override;
    void resized() override;

  private:
    void decode();

    struct Interpretation {
        juce::String concept, reading, explanation;
        std::map<juce::String, float> parameters;
        float confidence = 0.0f;
    };

    struct Card final : public juce::Component {
        void paint(juce::Graphics& g) override;
        void resized() override;
        Interpretation data;
        juce::TextButton applyButton{TRANS("Apply & A/B")};
    };

    IAppController& controller_;

    juce::Label titleLabel_;
    juce::TextEditor feedbackInput_;
    juce::TextButton decodeButton_{TRANS("Decode Feedback")};
    juce::Label statusLabel_;

    juce::Viewport viewport_;
    juce::Component content_;
    std::vector<std::unique_ptr<Card>> cards_;

    void applyInterpretation(const Interpretation& interp);
    /// Re-applies every cached palette colour. Called at construction and on
    /// each theme change.
    void applyThemeColours();

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FeedbackDecoderPanel)
};

} // namespace khensu
