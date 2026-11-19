// desktop-app/Source/ui/shared/RecommendationCard.h
#pragma once
#include <JuceHeader.h>
#include "KhensuTheme.h" // ThemeListener
#include <khensu/recommendations/Recommendation.h>

namespace khensu {

class IAppController;

// THREAD_CONTEXT(UIThread)
class RecommendationCard final : public juce::Component, public juce::SettableTooltipClient {
  public:
    explicit RecommendationCard(IAppController& controller);
    void setRecommendation(const Recommendation& rec);
    const Recommendation& recommendation() const noexcept { return rec_; }
    /// CE-AI-2: why the personal fix model moved this card ("you usually act
    /// on loudness fixes (5 of 6 applied)"); empty = no personal reason. Shown
    /// as a small "FOR YOU" tag and as the first line of the expanded detail.
    void setPersonalReason(const juce::String& reason);

    // Height helpers
    static int collapsedHeight() { return 44; }
    int preferredHeight() const
    {
        return expanded_ ? (collapsedHeight() + (tweaking_ ? 180 : detailHeight_)) : collapsedHeight();
    }

    // Priority badge colour - "priority"->accentRed, "recommended"->accentAmber, else->bgOverlay
    static juce::Colour badgeColourForPriority(const juce::String& priority);

    /// Plain-language measurement/evidence context for the expanded card.
    /// A non-empty abstention deliberately replaces the numeric precision:
    /// the latter is not a probability that the suggested intervention helps.
    static juce::String evidenceSummaryFor(const Recommendation& rec);

    /// CE-TRU-3: one ASCII line per Recommendation::evidence item
    /// ("True peak: 0.4 dBTP (limit -1.0 dBTP, genre norm)").
    static juce::String evidenceLinesFor(const Recommendation& rec);
    /// The issue code behind a card ("<code>_<fileId>" -> "<code>").
    static juce::String issueCodeFor(const Recommendation& rec);

    bool isExpanded() const { return expanded_; }

    std::function<void(const std::string& rec_id, bool acted_on)> onFeedbackClicked;
    /// P0-2: "Apply" renders the finding (with any tweaks). Set by the owner;
    /// it reports back through setApplyInFlight() / setApplyResult(). A card
    /// with nothing concrete to render - or no owner hook - shows "Mark done"
    /// instead, which calls onMarkDone: it records the finding as handled but
    /// is not a taste signal.
    std::function<void(const Recommendation& rec)> onApplyClicked;
    std::function<void(const std::string& rec_id)> onMarkDone;
    /// True when Apply renders (the finding has a renderable chain).
    bool appliesByRendering() const noexcept { return renderable_ && onApplyClicked != nullptr; }
    void setApplyInFlight(bool inFlight);
    void setApplyResult(bool rendered);
    juce::String applyButtonText() const { return applyButton_.getButtonText(); }
    std::function<std::optional<bool>(const std::string& rec_id)> onQueryFeedback;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& e) override;

  private:
    IAppController& controller_;
    Recommendation rec_;
    bool expanded_ = false;
    bool tweaking_ = false;
    juce::String detailText_;
    juce::String baseDetailText_; ///< detailText_ without the personal reason
    juce::String personalReason_;
    juce::String issueCode_; ///< CE-TRU-1: keys the calibration basis tag
    void updateDetailText();
    int detailHeight_ = 120;

    juce::TextButton applyButton_;
    bool renderable_ = false;
    enum class ApplyState { Idle, Rendering, Applied };
    ApplyState applyState_ = ApplyState::Idle;
    void updateApplyButton();
    juce::TextButton dismissButton_; // Renamed from ignoreButton_
    juce::TextButton tweakButton_;
    juce::TextButton reasoningButton_;
    /// CE-TRU-2: opens the counterfactual ("What would fix this?") call-out.
    juce::TextButton whatFixesButton_;
    juce::HyperlinkButton learnMoreButton_;

    juce::Slider freqSlider_;
    juce::Slider gainSlider_;
    juce::Slider qSlider_;
    juce::Label freqLabel_;
    juce::Label gainLabel_;
    juce::Label qLabel_;

    void setupSliders();
    /// Re-applies every cached palette colour. Called at construction and on
    /// each theme change.
    void applyThemeColours();

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RecommendationCard)
};

} // namespace khensu
