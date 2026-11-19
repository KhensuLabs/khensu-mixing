// desktop-app/Source/ui/shared/ChainReasoningView.h
#pragma once
#include <JuceHeader.h>
#include "KhensuTheme.h" // ThemeListener
#include <khensu/recommendations/Recommendation.h>
#include <array>
#include <memory>

namespace khensu {

/// Shared expandable "reasoning trace" accordion for a single Recommendation:
/// Detected -> Cause -> Fix -> Trade-off -> Alternative Strategies -> How to
/// Verify. Used from the Dashboard's RecommendationCard, SuggestionListPanel
/// (row click), and the Copilot chat (recommendation-referencing replies),
/// so the reasoning trace always reads the same way regardless of entry point.
///
/// THREAD_CONTEXT(UIThread)
class ChainReasoningView final : public juce::Component {
  public:
    ChainReasoningView();

    void setRecommendation(const Recommendation& rec);
    /// Adds the service's plain-language explanation (/copilot/explain) and,
    /// when the topic carries a common production myth, its clarification.
    /// Empty strings keep a section hidden. Cause / trade-off / verification
    /// fill only sections the recommendation left empty.
    void setPlainLanguage(const juce::String& explanation, const juce::String& mythClarification,
                          const juce::String& cause = {}, const juce::String& tradeOff = {},
                          const juce::String& howToVerify = {});
    void resized() override;
    void paint(juce::Graphics& g) override;

    /// Total height needed to show all sections at their current
    /// expanded/collapsed state - hosts embedding this directly (rather than
    /// inside a juce::Viewport) should size themselves to this after any
    /// section toggle (see onHeightChanged).
    int getContentHeight() const;

    /// Fired whenever a section is expanded/collapsed and getContentHeight()
    /// changes, so a host can re-layout (e.g. inside a Viewport, call
    /// setSize(width, getContentHeight()) then repaint the viewport).
    std::function<void()> onHeightChanged;

  private:
    struct Section final : public juce::Component {
        Section(const juce::String& label, juce::Colour accent);
        void resized() override;
        void paint(juce::Graphics& g) override;
        int preferredHeight() const;

        void setBody(const juce::String& text);
        void setExpanded(bool shouldExpand);

        juce::String label_;
        juce::Colour accent_;
        juce::TextButton headerButton_;
        juce::Label bodyLabel_;
        bool expanded_ = false;
        bool hasContent_ = false;

        std::function<void()> onToggled;

        /// Re-applies this section's cached palette colours (UI-01).
        void applyThemeColours();

        // Declared last - see ThemeListener's note on member ordering.
        ThemeListener themeListener_{[this] {
            applyThemeColours();
            repaint();
        }};

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Section)
    };

    static constexpr int kHeaderH = 26;
    static constexpr int kSectionGap = 4;

    // 0 In plain words, 1 Myth check, 2 Detected, 3 Cause, 4 Fix,
    // 5 Trade-off, 6 Alternative Strategies, 7 How to Verify.
    std::array<std::unique_ptr<Section>, 8> sections_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ChainReasoningView)
};

} // namespace khensu
