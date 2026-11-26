// desktop-app/Source/ui/Dashboard/ScoreCardPanel.cpp
#include "ScoreCardPanel.h"
#include "../shared/KhensuLookAndFeel.h"
#include "../shared/KhensuEmptyState.h"
#include "../shared/KhensuTheme.h"
#include <array>

namespace khensu {

namespace {
constexpr int kHeaderH = 30;
constexpr int kRowH = 34;

// Red (early-stage) -> amber -> teal (exceptional), matching the app's
// existing heat-gradient convention used elsewhere on the Dashboard.
juce::Colour tierColourFor(int bandIndex, int totalBands)
{
    const float t = totalBands > 1 ? static_cast<float>(bandIndex) / static_cast<float>(totalBands - 1) : 0.0f;
    if (t < 0.5f) return KhensuTheme::accentRed.interpolatedWith(KhensuTheme::accentAmber, t / 0.5f);
    return KhensuTheme::accentAmber.interpolatedWith(KhensuTheme::accentTeal, (t - 0.5f) / 0.5f);
}
} // anonymous namespace

const std::array<ScoreCardPanel::Band, 6>& ScoreCardPanel::bands()
{
    // Bounded readiness bands (heuristic technical/genre alignment) - not
    // quality/audience claims (2026-07-15 safety audit P0). Keep in sync with
    // MixScorecard.h's kTier* labels.
    static const std::array<Band, 6> kBands{{
        {0, 29, "Rough", "Well outside the genre targets"},
        {30, 49, "Early Draft", "Most dimensions far from target"},
        {50, 64, "Needs Work", "Many dimensions off target"},
        {65, 79, "Minor Issues", "Several dimensions off target"},
        {80, 89, "Near Target", "A few dimensions off target"},
        {90, 100, "Meets Targets", "Meets the genre's technical targets"},
    }};
    return kBands;
}

// Every cached palette colour on this component, in one place (UI-01).
void ScoreCardPanel::applyThemeColours()
{
    toggleBandsButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::bgOverlay);
    toggleBandsButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::textSecondary);
}

ScoreCardPanel::ScoreCardPanel()
{
    applyThemeColours();
    toggleBandsButton_.onClick = [this] {
        bandsVisible_ = !bandsVisible_;
        toggleBandsButton_.setButtonText(bandsVisible_ ? "Hide Score Bands" : "Show Score Bands");
        if (onLayoutChanged) onLayoutChanged();
        resized();
        repaint();
    };
    addAndMakeVisible(toggleBandsButton_);
}

void ScoreCardPanel::setScore(const MixScorecard& scorecard)
{
    hasData_ = true;
    score_ = scorecard.overall_score;
    tierLabel_ = scorecard.tier_label;
    tierAudience_ = scorecard.tier_audience;
    repaint();
}

void ScoreCardPanel::clear()
{
    hasData_ = false;
    repaint();
}

int ScoreCardPanel::preferredHeight() const
{
    constexpr int kHeroH = 110;
    return kHeroH + (bandsVisible_ ? kHeaderH + static_cast<int>(bands().size()) * kRowH + 8 : 0);
}

void ScoreCardPanel::resized() { toggleBandsButton_.setBounds(getWidth() - 154, 10, 140, 24); }

void ScoreCardPanel::paint(juce::Graphics& g)
{
    KhensuLookAndFeel::drawGlassPanel(g, getLocalBounds(), GlassPanelVariant::GlowAccent);

    auto b = getLocalBounds().reduced(KhensuTheme::sp4, KhensuTheme::sp3);

    auto header = b.removeFromTop(kHeaderH);
    g.setFont(KhensuTheme::sectionHeaderFont());
    g.setColour(KhensuTheme::textSecondary);
    g.drawText("READINESS (heuristic)", header.withTrimmedRight(150), juce::Justification::centredLeft, true);

    if (!hasData_) {
        KhensuEmptyState::paintInto(g, b, TRANS("Load a track to see the readiness indicator."), {}); // UXR-6
        return;
    }

    // Find the current tier's band index for colouring + the bands list highlight.
    const int roundedScore = static_cast<int>(std::round(score_));
    const auto& allBands = bands();
    int tierIdx = 0;
    for (int i = 0; i < static_cast<int>(allBands.size()); ++i)
        if (roundedScore >= allBands[i].lo && roundedScore <= allBands[i].hi) tierIdx = i;
    const juce::Colour tierColour = tierColourFor(tierIdx, static_cast<int>(allBands.size()));

    auto heroRow = b.removeFromTop(70);
    auto scoreArea = heroRow.removeFromLeft(150);
    g.setFont(KhensuTheme::metricFont(type::hero));
    g.setColour(KhensuTheme::textPrimary);
    g.drawText(juce::String(static_cast<int>(std::round(score_))), scoreArea.removeFromLeft(90),
               juce::Justification::centredLeft, false);
    g.setFont(KhensuTheme::labelFont(type::body));
    g.setColour(KhensuTheme::textTertiary);
    g.drawText("/ 100", scoreArea, juce::Justification::centredLeft, false);

    heroRow.removeFromLeft(8);
    auto tierArea = heroRow;
    g.setFont(KhensuTheme::bodyFont(type::title).boldened());
    g.setColour(tierColour);
    g.drawText(tierLabel_, tierArea.removeFromTop(24), juce::Justification::centredLeft, false);
    g.setFont(KhensuTheme::bodyFont(type::secondary));
    g.setColour(KhensuTheme::textSecondary);
    g.drawText(tierAudience_, tierArea.removeFromTop(20), juce::Justification::centredLeft, false);

    if (!bandsVisible_) return;

    b.removeFromTop(6);
    g.setColour(KhensuTheme::borderSubtle);
    g.drawHorizontalLine(b.getY(), (float)b.getX(), (float)b.getRight());
    b.removeFromTop(8);

    for (int i = 0; i < static_cast<int>(allBands.size()); ++i) {
        const auto& band = allBands[i];
        auto row = b.removeFromTop(kRowH);
        const bool isCurrent = (i == tierIdx);

        if (isCurrent) {
            g.setColour(tierColourFor(i, static_cast<int>(allBands.size())).withAlpha(0.12f));
            g.fillRoundedRectangle(row.toFloat(), 4.0f);
        }

        auto rangeArea = row.removeFromLeft(70).reduced(6, 4);
        g.setFont(KhensuTheme::monoFont(type::caption));
        g.setColour(isCurrent ? tierColourFor(i, static_cast<int>(allBands.size())) : KhensuTheme::textTertiary);
        g.drawText(juce::String(band.lo) + "-" + juce::String(band.hi), rangeArea, juce::Justification::centredLeft,
                   false);

        auto labelArea = row.removeFromLeft(160).reduced(2, 4);
        g.setFont(KhensuTheme::bodyFont(type::secondary).boldened());
        g.setColour(isCurrent ? KhensuTheme::textPrimary : KhensuTheme::textSecondary);
        g.drawText(band.label, labelArea, juce::Justification::centredLeft, false);

        auto audienceArea = row.reduced(2, 4);
        g.setFont(KhensuTheme::bodyFont(type::caption));
        g.setColour(KhensuTheme::textTertiary);
        g.drawText(band.audience, audienceArea.withTrimmedRight(isCurrent ? 60 : 0), juce::Justification::centredLeft,
                   false);

        if (isCurrent) {
            g.setFont(KhensuTheme::bodyFont(type::secondary).boldened());
            g.setColour(tierColourFor(i, static_cast<int>(allBands.size())));
            g.drawText("<- you", audienceArea, juce::Justification::centredRight, false);
        }
    }
}

} // namespace khensu
