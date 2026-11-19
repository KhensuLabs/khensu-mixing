// desktop-app/Source/ui/Dashboard/KhensuVerdictCard.h
// Analysis verdict card (kimi audit P1): the single scannable headline that
// replaces "a wall of 30 panels" as the first thing a user reads after
// analysis. Shows a readiness %, the severe/moderate/minor/strength counts
// colour-coded to the mix-note scheme, a one-line "start here" pointer to
// the top issue, and a primary CTA that jumps to it.
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include "../shared/KhensuTheme.h"
#include "../shared/ButtonRole.h"
#include <khensu/analysis/AnalysisResult.h>
#include <khensu/recommendations/RecommendationType.h>

namespace khensu {

class KhensuVerdictCard final : public juce::Component {
  public:
    static constexpr int kHeight = 58;

    KhensuVerdictCard()
    {
        fixButton_.setButtonText(TRANS("Fix top issue ->"));
        applyThemeColours();
        fixButton_.onClick = [this] {
            if (onFixClicked) onFixClicked();
        };
        button_role::markPrimary(fixButton_); // UX-4
        addChildComponent(fixButton_);
    }

    std::function<void()> onFixClicked;

    /// `readiness` 0-100 from the existing scorecard; recommendations drive
    /// the counts and the "start here" pointer.
    void setData(const AnalysisResult& r, float readiness)
    {
        readiness_ = readiness;
        severe_ = moderate_ = minor_ = strengths_ = 0;
        topIssue_.clear();
        float topSev = -1.0f;
        for (const auto& rec : r.recommendations) {
            if (rec.type == RecommendationType::Strength) {
                ++strengths_;
                continue;
            }
            if (rec.severity >= 66.0f)
                ++severe_;
            else if (rec.severity >= 33.0f)
                ++moderate_;
            else
                ++minor_;
            if (rec.severity > topSev) {
                topSev = rec.severity;
                topIssue_ = juce::String(rec.what.empty() ? rec.suggested_action : rec.what);
            }
        }
        hasData_ = true;
        fixButton_.setVisible(severe_ + moderate_ + minor_ > 0);
        resized();
        repaint();
    }

    void setEmpty()
    {
        hasData_ = false;
        fixButton_.setVisible(false);
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat().reduced(0.5f);
        g.setColour(KhensuTheme::bgRaised.withAlpha(0.92f));
        g.fillRoundedRectangle(b, 6.0f);
        g.setColour(KhensuTheme::borderActive);
        g.drawRoundedRectangle(b, 6.0f, 1.0f);

        auto inner = getLocalBounds().reduced(14, 8);
        if (!hasData_) {
            g.setColour(KhensuTheme::textTertiary);
            g.setFont(KhensuTheme::bodyFont(12.0f));
            g.drawText("Load a track to see its readiness verdict.", inner, juce::Justification::centredLeft, false);
            return;
        }

        // Readiness % (left, large), colour-graded.
        const juce::Colour rc = readiness_ >= 80.0f   ? KhensuTheme::accentGreen
                                : readiness_ >= 55.0f ? KhensuTheme::accentAmber
                                                      : KhensuTheme::accentRed;
        auto scoreArea = inner.removeFromLeft(72);
        g.setFont(KhensuTheme::metricFont(26.0f));
        g.setColour(rc);
        g.drawText(juce::String((int)std::round(readiness_)) + "%", scoreArea.removeFromTop(30),
                   juce::Justification::centredLeft, false);
        g.setFont(KhensuTheme::labelFont(8.5f));
        g.setColour(KhensuTheme::textTertiary);
        // The mastering-readiness score, named apart from the scorecard's
        // "Readiness (heuristic)" overall score - both said "readiness" and
        // showed different numbers (55% vs 72) on the same screen (2026-11-18).
        g.drawText(TRANS("MASTER-READY"), scoreArea, juce::Justification::topLeft, false);

        inner.removeFromLeft(10);
        if (fixButton_.isVisible()) inner.removeFromRight(140);

        // Counts line + "start here".
        auto top = inner.removeFromTop(inner.getHeight() / 2);
        auto drawCount = [&](juce::Rectangle<int>& area, int n, const char* label, juce::Colour c) {
            if (n <= 0) return;
            const juce::String t = juce::String(n) + " " + label;
            g.setFont(KhensuTheme::labelFont(10.5f).boldened());
            const int w = 14 + juce::GlyphArrangement::getStringWidthInt(KhensuTheme::labelFont(10.5f), t);
            g.setColour(c);
            g.drawText(t, area.removeFromLeft(w), juce::Justification::centredLeft, false);
            area.removeFromLeft(6);
        };
        drawCount(top, severe_, "severe", KhensuTheme::accentRed);
        drawCount(top, moderate_, "moderate", KhensuTheme::accentAmber);
        drawCount(top, minor_, "minor", KhensuTheme::accentBlue);
        drawCount(top, strengths_, "strengths", KhensuTheme::accentGreen);
        if (severe_ + moderate_ + minor_ + strengths_ == 0) {
            g.setFont(KhensuTheme::labelFont(10.5f));
            g.setColour(KhensuTheme::textSecondary);
            g.drawText("No issues flagged.", top, juce::Justification::centredLeft, false);
        }

        g.setFont(KhensuTheme::bodyFont(11.0f));
        g.setColour(KhensuTheme::textSecondary);
        g.drawText(topIssue_.isNotEmpty() ? "Start here: " + topIssue_ : "This mix is in good shape.", inner,
                   juce::Justification::centredLeft, true);
    }

    void resized() override
    {
        fixButton_.setBounds(getLocalBounds().reduced(14, 12).removeFromRight(126).withSizeKeepingCentre(126, 30));
    }

  private:
    juce::TextButton fixButton_;
    float readiness_ = 0.0f;
    int severe_ = 0, moderate_ = 0, minor_ = 0, strengths_ = 0;
    juce::String topIssue_;
    bool hasData_ = false;

    /// Every cached palette colour on this card, in one place (UI-01).
    void applyThemeColours()
    {
        fixButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::accentTeal.withAlpha(0.22f));
        fixButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::accentTeal);
    }

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(KhensuVerdictCard)
};

} // namespace khensu
