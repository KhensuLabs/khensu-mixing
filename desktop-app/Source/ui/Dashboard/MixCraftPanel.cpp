// desktop-app/Source/ui/Dashboard/MixCraftPanel.cpp
#include "MixCraftPanel.h"
#include "../shared/KhensuLookAndFeel.h"
#include "../shared/KhensuTheme.h"

namespace khensu {

namespace {
constexpr int kHeaderH = 44;
constexpr int kRowH = 28;
constexpr int kSubRowH = 22;

juce::Colour scoreColour(float score)
{
    if (score >= 80.0f) return KhensuTheme::accentTeal;
    if (score >= 60.0f) return KhensuTheme::accentAmber;
    return KhensuTheme::accentRed;
}
} // anonymous namespace

void MixCraftPanel::setScorecard(const MixScorecard& scorecard)
{
    hasData_ = true;
    scorecard_ = scorecard;
    repaint();
}

void MixCraftPanel::clear()
{
    hasData_ = false;
    repaint();
}

int MixCraftPanel::preferredHeight() const
{
    int rows = 7;    // fixed category count
    int subRows = 2; // Technical Integrity's Clipping / Noise & Artefacts
    return kHeaderH + rows * kRowH + subRows * kSubRowH + KhensuTheme::sp3 * 2;
}

void MixCraftPanel::paint(juce::Graphics& g)
{
    KhensuLookAndFeel::drawGlassPanel(g, getLocalBounds(), GlassPanelVariant::Default);

    auto b = getLocalBounds().reduced(KhensuTheme::sp4, KhensuTheme::sp3);

    auto header = b.removeFromTop(kHeaderH - 8);
    g.setFont(KhensuTheme::sectionHeaderFont());
    g.setColour(KhensuTheme::textSecondary);
    g.drawText("MIX CRAFT", header.removeFromLeft(160), juce::Justification::centredLeft, true);

    if (!hasData_) {
        g.setFont(KhensuTheme::bodyFont(13.0f));
        g.setColour(KhensuTheme::textTertiary);
        g.drawText("Load a track to see the category breakdown.", b, juce::Justification::centredLeft);
        return;
    }

    g.setFont(KhensuTheme::metricFont(20.0f));
    g.setColour(scoreColour(scorecard_.overall_score));
    g.drawText(juce::String(static_cast<int>(std::round(scorecard_.overall_score))) + "/100", header.removeFromLeft(90),
               juce::Justification::centredLeft, false);

    const int analyzedCount =
        static_cast<int>(std::round(scorecard_.completeness * static_cast<float>(scorecard_.categories.size())));
    g.setFont(KhensuTheme::bodyFont(11.0f));
    g.setColour(KhensuTheme::textTertiary);
    g.drawText("(" + juce::String(analyzedCount) + "/" + juce::String((int)scorecard_.categories.size()) +
                   " categories analyzed)",
               header, juce::Justification::centredRight, false);

    b.removeFromTop(4);
    g.setColour(KhensuTheme::borderSubtle);
    g.drawHorizontalLine(b.getY(), (float)b.getX(), (float)b.getRight());
    b.removeFromTop(6);

    for (const auto& cat : scorecard_.categories) {
        auto row = b.removeFromTop(kRowH);

        auto nameArea = row.removeFromLeft(220);
        g.setFont(KhensuTheme::bodyFont(12.5f).boldened());
        g.setColour(KhensuTheme::textPrimary);
        g.drawText(cat.name, nameArea, juce::Justification::centredLeft, true);
        g.setFont(KhensuTheme::bodyFont(10.5f));
        g.setColour(KhensuTheme::textTertiary);
        g.drawText("(" + juce::String(static_cast<int>(std::round(cat.weight * 100.0f))) + "%)", nameArea,
                   juce::Justification::centredRight, false);

        if (!cat.tag.empty()) {
            auto tagArea = row.removeFromRight(90).reduced(2);
            const bool isStrongest = cat.tag == ScorecardCategory::kTagStrongest;
            const juce::Colour tagColour = isStrongest ? KhensuTheme::accentTeal : KhensuTheme::accentAmber;
            g.setColour(tagColour.withAlpha(0.15f));
            g.fillRoundedRectangle(tagArea.toFloat(), 4.0f);
            g.setColour(tagColour);
            g.setFont(KhensuTheme::bodyFont(10.0f).boldened());
            g.drawText(cat.tag, tagArea, juce::Justification::centred, false);
        }

        auto scoreArea = row.removeFromRight(60);
        if (cat.available) {
            g.setFont(KhensuTheme::bodyFont(14.0f).boldened());
            g.setColour(scoreColour(cat.score));
            g.drawText(juce::String(static_cast<int>(std::round(cat.score))), scoreArea,
                       juce::Justification::centredRight, false);
        } else {
            g.setFont(KhensuTheme::bodyFont(10.5f).italicised());
            g.setColour(KhensuTheme::textTertiary);
            g.drawText("Not yet analyzed", scoreArea.withTrimmedLeft(-60), juce::Justification::centredRight, false);
        }

        // Technical Integrity gets its Clipping / Noise & Artefacts sub-scores
        // shown indented directly beneath it.
        for (const auto& sub : cat.sub_categories) {
            auto subRow = b.removeFromTop(kSubRowH);
            subRow.removeFromLeft(20); // indent
            auto subNameArea = subRow.removeFromLeft(200);
            g.setFont(KhensuTheme::bodyFont(11.0f));
            g.setColour(KhensuTheme::textSecondary);
            g.drawText(sub.name, subNameArea, juce::Justification::centredLeft, true);
            g.setFont(KhensuTheme::bodyFont(9.5f));
            g.setColour(KhensuTheme::textTertiary);
            g.drawText("(" + juce::String(static_cast<int>(std::round(sub.weight * 100.0f))) + "%)", subNameArea,
                       juce::Justification::centredRight, false);

            auto subScoreArea = subRow.removeFromRight(60);
            g.setFont(KhensuTheme::bodyFont(12.0f).boldened());
            g.setColour(scoreColour(sub.score));
            g.drawText(juce::String(static_cast<int>(std::round(sub.score))), subScoreArea,
                       juce::Justification::centredRight, false);
        }
    }
}

} // namespace khensu
