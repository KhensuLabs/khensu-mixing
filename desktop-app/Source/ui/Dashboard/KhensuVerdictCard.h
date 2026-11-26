// desktop-app/Source/ui/Dashboard/KhensuVerdictCard.h
// The Dashboard's headline (kimi audit P1, OVH-1): the first thing a user
// reads after analysis. ONE number - Readiness, the scorecard's overall score,
// whose categories the Mix Craft card breaks down - with its band, the
// severe / moderate / minor / strength counts of the same findings the score
// was built from, and the one thing to fix ("Start here"), whose button fixes
// exactly that issue (VerdictModel.h).
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include "../shared/KhensuTheme.h"
#include "../shared/ButtonRole.h"
#include "../shared/ValueAnimator.h"
#include "VerdictModel.h"
#include <khensu/recommendations/MixScorecard.h>

namespace khensu {

class KhensuVerdictCard final : public juce::Component {
  public:
    static constexpr int kHeight = 112;

    KhensuVerdictCard()
    {
        applyThemeColours();
        fixButton_.onClick = [this] {
            if (start_.fixable) {
                if (onFixClicked) onFixClicked();
            } else if (onReviewClicked) {
                onReviewClicked();
            }
        };
        button_role::markPrimary(fixButton_); // UX-4
        addChildComponent(fixButton_);
    }

    /// The button fixes the issue "Start here" names (the issue queue's top
    /// fixable issue).
    std::function<void()> onFixClicked;
    /// No issue has a one-click fix: the button opens the issue list instead.
    std::function<void()> onReviewClicked;
    /// ENH-7: the "Start here" finding was clicked (its recommendation id) -
    /// the Dashboard shows that finding's card.
    std::function<void(const std::string& recId)> onStartHereClicked;

    /// Where "Start here" is drawn (the click target); empty when none.
    juce::Rectangle<int> startHereArea() const noexcept { return startHereArea_; }

    void mouseMove(const juce::MouseEvent& e) override
    {
        const bool over = startHereArea_.contains(e.getPosition()) && start_.recId.size() > 0 && onStartHereClicked;
        setMouseCursor(over ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
        if (over != hoverStart_) {
            hoverStart_ = over;
            repaint(startHereArea_);
        }
    }
    void mouseExit(const juce::MouseEvent&) override
    {
        if (!hoverStart_) return;
        hoverStart_ = false;
        repaint(startHereArea_);
    }
    void mouseUp(const juce::MouseEvent& e) override
    {
        if (e.mouseWasDraggedSinceMouseDown() || !startHereArea_.contains(e.getPosition())) return;
        if (hasData_ && !start_.recId.empty() && onStartHereClicked) onStartHereClicked(start_.recId);
    }

    /// `scopeNote`: what the counts cover when the issue queue already
    /// describes a fixed version (RUN-8), else empty.
    void setData(const MixScorecard& scorecard, const verdict::Counts& counts, const verdict::StartHere& start,
                 const juce::String& scopeNote = {})
    {
        scopeNote_ = scopeNote;
        score_.setTarget(scorecard.overall_score, hasData_); // UI-7: a new score moves from the old one
        tierLabel_ = juce::String(scorecard.tier_label);
        tierText_ = juce::String(scorecard.tier_audience);
        counts_ = counts;
        start_ = start;
        hasData_ = true;
        fixButton_.setButtonText(start_.fixable ? TRANS("Fix this") : TRANS("Review issues"));
        fixButton_.setVisible(start_.text.isNotEmpty());
        resized();
        repaint();
    }

    void setEmpty()
    {
        hasData_ = false;
        score_.reset();
        fixButton_.setVisible(false);
        repaint();
    }

    /// The headline as a screen reader / test reads it.
    juce::String headlineText() const
    {
        if (!hasData_) return {};
        return TRANS("Readiness") + " " + juce::String(juce::roundToInt(score_.target())) + " - " + tierLabel_;
    }
    const verdict::StartHere& startHere() const noexcept { return start_; }
    const verdict::Counts& counts() const noexcept { return counts_; }

    void paint(juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat().reduced(0.5f);
        g.setColour(KhensuTheme::bgSurface);
        g.fillRoundedRectangle(b, 8.0f);
        g.setColour(KhensuTheme::borderSubtle);
        g.drawRoundedRectangle(b, 8.0f, 1.0f);

        auto inner = getLocalBounds().reduced(KhensuTheme::panelPadding, KhensuTheme::sp4);
        if (!hasData_) {
            g.setColour(KhensuTheme::textTertiary);
            g.setFont(KhensuTheme::bodyFont());
            g.drawText(TRANS("Load a track to see its readiness."), inner, juce::Justification::centredLeft, false);
            return;
        }

        // Left: the one number. Label above, value the largest text on the card.
        auto scoreCol = inner.removeFromLeft(196);
        g.setFont(KhensuTheme::labelFont(KhensuTheme::kFontLabel));
        g.setColour(KhensuTheme::textSecondary);
        g.drawText(TRANS("Readiness"), scoreCol.removeFromTop(16), juce::Justification::centredLeft, false);

        auto valueRow = scoreCol.removeFromTop(40);
        const juce::String value = juce::String(juce::roundToInt(score_.value()));
        const auto valueFont = KhensuTheme::numericFont(type::hero);
        g.setFont(valueFont);
        g.setColour(KhensuTheme::textPrimary);
        const int valueW = juce::GlyphArrangement::getStringWidthInt(valueFont, value) + 4;
        g.drawText(value, valueRow.removeFromLeft(valueW), juce::Justification::centredLeft, false);
        g.setFont(KhensuTheme::numericFont(KhensuTheme::kFontLabel));
        g.setColour(KhensuTheme::textTertiary);
        g.drawText("/ 100", valueRow.withTrimmedTop(14), juce::Justification::centredLeft, false);

        // Band: a grade dot, the band name and what it means.
        auto bandRow = scoreCol.removeFromTop(20);
        g.setColour(verdict::colourFor(score_.target()));
        g.fillEllipse(juce::Rectangle<float>(8.0f, 8.0f).withCentre({(float)bandRow.getX() + 4.0f,
                                                                      (float)bandRow.getCentreY()}));
        g.setFont(KhensuTheme::bodyFont().boldened());
        g.setColour(KhensuTheme::textPrimary);
        g.drawText(tierLabel_, bandRow.withTrimmedLeft(14), juce::Justification::centredLeft, true);

        inner.removeFromLeft(KhensuTheme::sp5);
        // Hairline between the score and the advice.
        g.setColour(KhensuTheme::borderSubtle);
        g.drawVerticalLine(inner.getX() - KhensuTheme::sp3, (float)inner.getY(), (float)inner.getBottom());

        if (fixButton_.isVisible()) inner.removeFromRight(kButtonW + KhensuTheme::sp4);

        // Right of the rule: the band's meaning, the counts, then the one thing to fix.
        g.setFont(KhensuTheme::labelFont(KhensuTheme::kFontLabel));
        g.setColour(KhensuTheme::textSecondary);
        g.drawText(tierText_, inner.removeFromTop(16), juce::Justification::centredLeft, true);

        auto countsRow = inner.removeFromTop(22);
        auto drawCount = [&](int n, const juce::String& label, juce::Colour c) {
            if (n <= 0) return;
            g.setColour(c);
            g.fillEllipse(juce::Rectangle<float>(6.0f, 6.0f).withCentre({(float)countsRow.getX() + 3.0f,
                                                                          (float)countsRow.getCentreY()}));
            countsRow.removeFromLeft(10);
            const juce::String t = juce::String(n) + " " + label;
            const auto f = KhensuTheme::numericFont(KhensuTheme::kFontLabel);
            g.setFont(f);
            g.setColour(KhensuTheme::textSecondary);
            const int w = juce::GlyphArrangement::getStringWidthInt(f, t) + 4;
            g.drawText(t, countsRow.removeFromLeft(w), juce::Justification::centredLeft, false);
            countsRow.removeFromLeft(KhensuTheme::sp4);
        };
        drawCount(counts_.severe, TRANS("severe"), KhensuTheme::accentRed);
        drawCount(counts_.moderate, TRANS("moderate"), KhensuTheme::accentAmber);
        drawCount(counts_.minor, TRANS("minor"), KhensuTheme::accentBlue);
        drawCount(counts_.strengths, TRANS("strengths"), KhensuTheme::accentGreen);
        if (counts_.issues() + counts_.strengths == 0) {
            g.setFont(KhensuTheme::labelFont(KhensuTheme::kFontLabel));
            g.setColour(KhensuTheme::textSecondary);
            g.drawText(TRANS("No issues flagged."), countsRow, juce::Justification::centredLeft, false);
        } else if (scopeNote_.isNotEmpty()) {
            g.setFont(KhensuTheme::labelFont(KhensuTheme::kFontLabel));
            g.setColour(KhensuTheme::textTertiary);
            g.drawText(scopeNote_, countsRow, juce::Justification::centredLeft, true);
        }

        inner.removeFromTop(KhensuTheme::sp1);
        auto startRow = inner.removeFromTop(36);
        startHereArea_ = {};
        if (start_.text.isNotEmpty()) {
            startHereArea_ = startRow;
            g.setFont(KhensuTheme::labelFont(KhensuTheme::kFontLabel));
            g.setColour(KhensuTheme::textTertiary);
            juce::String caption =
                start_.onCurrentVersion ? TRANS("Start here - on the current version") : TRANS("Start here");
            if (onStartHereClicked) caption << " - " << TRANS("click to see its card");
            g.drawText(caption, startRow.removeFromTop(14), juce::Justification::centredLeft, false);
            g.setFont(KhensuTheme::bodyFont());
            g.setColour(hoverStart_ ? KhensuTheme::accentTeal : KhensuTheme::textPrimary);
            g.drawFittedText(start_.text, startRow, juce::Justification::centredLeft, 1, 0.9f);
            if (hoverStart_) {
                const int textW = juce::jmin(startRow.getWidth(),
                                             juce::GlyphArrangement::getStringWidthInt(KhensuTheme::bodyFont(), start_.text));
                g.fillRect(startRow.getX(), startRow.getCentreY() + 9, textW, 1);
            }
        } else {
            g.setFont(KhensuTheme::bodyFont());
            g.setColour(KhensuTheme::textSecondary);
            g.drawText(TRANS("Nothing open - this mix is in good shape."), startRow,
                       juce::Justification::centredLeft, true);
        }
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced(KhensuTheme::panelPadding, 0);
        fixButton_.setBounds(r.removeFromRight(kButtonW).withSizeKeepingCentre(kButtonW, KhensuTheme::buttonHLarge));
    }

  private:
    static constexpr int kButtonW = 148;

    juce::TextButton fixButton_{TRANS("Fix this")};
    verdict::Counts counts_;
    verdict::StartHere start_;
    juce::String tierLabel_, tierText_, scopeNote_;
    bool hasData_ = false;
    juce::Rectangle<int> startHereArea_; ///< ENH-7: set by paint()
    bool hoverStart_ = false;

    /// Every cached palette colour on this card, in one place (UI-01). The
    /// primary role (UX-4) draws the fill; nothing else is cached.
    void applyThemeColours() {}

    // After the members its repaints read.
    ValueAnimator score_{[this] { repaint(); }};

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(KhensuVerdictCard)
};

} // namespace khensu
