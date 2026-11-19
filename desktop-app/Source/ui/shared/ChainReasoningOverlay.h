// desktop-app/Source/ui/shared/ChainReasoningOverlay.h
#pragma once
#include <JuceHeader.h>
#include "KhensuTheme.h" // ThemeListener
#include "ChainReasoningView.h"
#include "ModalCardHost.h"
#include <khensu/recommendations/Recommendation.h>

namespace khensu {

/// Modal overlay hosting a ChainReasoningView for one Recommendation -
/// the click-through destination used from RecommendationCard (Dashboard),
/// SuggestionListPanel row clicks, and chat replies that reference a
/// specific recommendation. Mirrors ExplainerOverlay/ExplainerPopup's
/// dim-backdrop + centered-card pattern.
///
/// THREAD_CONTEXT(UIThread)
class ChainReasoningOverlay final : public ModalCardHost {
  public:
    explicit ChainReasoningOverlay(const Recommendation& rec);
    ~ChainReasoningOverlay() override = default;

    void resized() override;

    /// See ChainReasoningView::setPlainLanguage.
    void setPlainLanguage(const juce::String& explanation, const juce::String& mythClarification,
                          const juce::String& cause, const juce::String& tradeOff, const juce::String& howToVerify)
    {
        reasoningView_.setPlainLanguage(explanation, mythClarification, cause, tradeOff, howToVerify);
    }

    std::function<void()> onDismiss;

  protected:
    juce::Rectangle<int> cardBounds() const override { return card_.getBounds(); }
    void dismiss() override
    {
        if (onDismiss) onDismiss();
    }

  private:
    juce::Component card_;
    juce::Label titleLabel_;
    juce::TextButton closeButton_;
    juce::Viewport viewport_;
    ChainReasoningView reasoningView_;
    /// Re-applies every cached palette colour. Called at construction and on
    /// each theme change.
    void applyThemeColours();

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ChainReasoningOverlay)
};

} // namespace khensu
