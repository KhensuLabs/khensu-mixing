// desktop-app/Source/ui/shared/FeatureHomeLink.h
// ENH-8: one home per feature. Where a screen used to repeat a feature whose
// home is another screen (the mastering agent, the streaming platform table,
// stem energy and masking, gain staging), it shows this compact card instead:
// where the feature lives, an optional one-line readout, and one button that
// goes there.
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include "KhensuLookAndFeel.h"
#include "KhensuTheme.h"
#include "../../services/IShell.h"
#include <functional>
#include <string>

namespace khensu {

class FeatureHomeLink final : public juce::Component {
  public:
    static constexpr int kHeadlineH = 20;
    static constexpr int kBodyH = 36; ///< two lines of body text
    static constexpr int kReadoutH = 18;
    /// Narrower than this, the button goes under the text, full width.
    static constexpr int kNarrowW = 420;

    FeatureHomeLink(const juce::String& headline, const juce::String& body, const juce::String& buttonText,
                    std::function<void()> go)
        : go_(std::move(go))
    {
        headline_.setText(headline, juce::dontSendNotification);
        headline_.setFont(KhensuTheme::sectionHeaderFont().boldened());
        body_.setText(body, juce::dontSendNotification);
        body_.setFont(KhensuTheme::bodyFont(KhensuTheme::kFontBody));
        body_.setJustificationType(juce::Justification::topLeft);
        readout_.setFont(KhensuTheme::numericFont(KhensuTheme::kFontBody));
        applyThemeColours();
        addAndMakeVisible(headline_);
        addAndMakeVisible(body_);
        addChildComponent(readout_);
        goButton_.setButtonText(buttonText);
        goButton_.setTooltip(headline);
        goButton_.onClick = [this] {
            if (go_) go_();
        };
        addAndMakeVisible(goButton_);
        setTitle(headline);
    }

    /// One line of numbers beside the link (empty hides it).
    void setReadout(const juce::String& text)
    {
        readout_.setText(text, juce::dontSendNotification);
        readout_.setVisible(text.isNotEmpty());
        resized();
    }
    juce::String readout() const { return readout_.getText(); }

    /// The height the card needs at `width` (the button sits beside the text,
    /// or under it on a narrow card).
    int preferredHeight(int width) const noexcept
    {
        int text = kHeadlineH + kBodyH + (readout_.isVisible() ? kReadoutH : 0);
        if (width < kNarrowW) text += KhensuTheme::sp1 + KhensuTheme::buttonH;
        return 2 * KhensuTheme::sp2 + juce::jmax(text, KhensuTheme::buttonH);
    }
    juce::TextButton& button() noexcept { return goButton_; }

    void paint(juce::Graphics& g) override
    {
        KhensuLookAndFeel::drawGlassPanel(g, getLocalBounds(), GlassPanelVariant::Default);
    }

    void resized() override
    {
        auto b = getLocalBounds().reduced(KhensuTheme::sp3, KhensuTheme::sp2);
        if (getWidth() < kNarrowW) {
            goButton_.setBounds(b.removeFromBottom(KhensuTheme::buttonH));
            b.removeFromBottom(KhensuTheme::sp1);
        } else {
            const int bw = juce::jmin(200, b.getWidth() / 3);
            goButton_.setBounds(b.removeFromRight(bw).withSizeKeepingCentre(bw, KhensuTheme::buttonH));
            b.removeFromRight(KhensuTheme::sp3);
        }
        headline_.setBounds(b.removeFromTop(kHeadlineH));
        if (readout_.isVisible()) readout_.setBounds(b.removeFromBottom(kReadoutH));
        body_.setBounds(b);
    }

  private:
    void applyThemeColours()
    {
        headline_.setColour(juce::Label::textColourId, KhensuTheme::textPrimary);
        body_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
        readout_.setColour(juce::Label::textColourId, KhensuTheme::textPrimary);
    }

    std::function<void()> go_;
    juce::Label headline_, body_, readout_;
    juce::TextButton goButton_;

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FeatureHomeLink)
};

namespace feature_home {

/// The Analysis screen, for a link to one of its cards (a Zone 3 card id such
/// as "streaming_norm" or "taxonomy_masking"). ENH-7's reveal goes by
/// finding, so a card that shows a finding is revealed through one of its
/// codes (the card scrolled to, its group tab selected); any other card, or
/// no session, opens the screen. Every ENH-8 link to an Analysis card goes
/// through here, so a reveal by card id is a one-line change.
inline void showAnalysisCard(IShell& shell, const std::string& cardId)
{
    static constexpr std::pair<const char*, const char*> kFindingForCard[] = {
        {"taxonomy_masking", "masking_collision"},
        {"eq_curve", "resonance_hotspot"},
    };
    for (const auto& [card, finding] : kFindingForCard)
        if (cardId == card && shell.revealCardForFinding(finding)) return;
    shell.showScreen(ScreenType::Dashboard);
}

} // namespace feature_home

} // namespace khensu
