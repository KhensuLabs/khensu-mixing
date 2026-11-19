#pragma once
#include <JuceHeader.h>
#include "KhensuTheme.h" // ThemeListener
#include <khensu/common/ThreadContext.h>

namespace khensu {

// THREAD_CONTEXT(khensu::ThreadContext::UIThread)
class ExplainerPopup final : public juce::Component {
  public:
    ExplainerPopup(const juce::String& title, const juce::String& category, const juce::String& description,
                   const juce::String& systemBrainFeatureId = {});
    ~ExplainerPopup() override = default;

    void paint(juce::Graphics& g) override;
    void resized() override;

    std::function<void()> onClose;
    std::function<void()> onViewSystemBrain;

  private:
    juce::Label titleLabel;
    juce::Label categoryLabel;
    juce::Label descLabel;
    juce::TextButton closeButton;
    juce::TextButton systemBrainButton;
    juce::String systemBrainFeatureId_;
    /// Re-applies every cached palette colour. Called at construction and on
    /// each theme change.
    void applyThemeColours();

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ExplainerPopup)
};

} // namespace khensu
