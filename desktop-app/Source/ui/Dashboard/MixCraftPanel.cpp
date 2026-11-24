// desktop-app/Source/ui/Dashboard/MixCraftPanel.cpp
#include "MixCraftPanel.h"
#include "../shared/KhensuLookAndFeel.h"
#include "../shared/KhensuTheme.h"
#include "VerdictModel.h"

namespace khensu {

namespace {
constexpr int kHeaderH = 36;
constexpr int kSubtitleH = 18;
constexpr int kRowH = 32;
constexpr int kSubRowH = 24;
constexpr int kPadX = KhensuTheme::sp5;
constexpr int kPadY = KhensuTheme::sp4;
constexpr int kWeightControlW = 220;
constexpr int kScoreW = 44;

/// The fixed category count and Technical Integrity's two sub-scores.
constexpr int kRows = 7;
constexpr int kSubRows = 2;
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

void MixCraftPanel::setSpread(const GenreSpread::Entry* entry, const juce::String& genreName)
{
    spread_.clear();
    spreadName_ = genreName;
    spreadTracks_ = entry != nullptr ? entry->tracks : 0;
    if (entry != nullptr) spread_ = entry->scorecard;
    repaint();
}

juce::String MixCraftPanel::rowReadout(const ScorecardCategory& cat) const
{
    juce::String text = juce::String(cat.name) + ": ";
    text << (cat.available ? juce::String(juce::roundToInt(cat.score)) : TRANS("not yet analysed"));
    const auto it = spread_.find(cat.id);
    if (it != spread_.end()) {
        const auto& p = it->second;
        text << " - " << TRANS("released") << " " << spreadName_ << ": " << TRANS("middle half") << " "
             << juce::roundToInt(p[GenreSpread::kP25]) << "-" << juce::roundToInt(p[GenreSpread::kP75]) << ", "
             << TRANS("median") << " " << juce::roundToInt(p[GenreSpread::kP50]);
    }
    return text;
}

void MixCraftPanel::mouseMove(const juce::MouseEvent& e)
{
    // VIZ-U3: the row under the pointer, its score and the released spread.
    for (const auto& [area, id] : rowAreas_)
        if (area.contains(e.getPosition()))
            for (const auto& cat : scorecard_.categories)
                if (cat.id == id) {
                    setTooltip(rowReadout(cat));
                    return;
                }
    setTooltip({});
}

int MixCraftPanel::preferredHeight() const
{
    return 2 * kPadY + kHeaderH + kSubtitleH + KhensuTheme::sp3 + kRows * kRowH + kSubRows * kSubRowH;
}

juce::Rectangle<int> MixCraftPanel::weightControlArea() const
{
    auto header = getLocalBounds().reduced(kPadX, kPadY).removeFromTop(kHeaderH);
    const int w = juce::jmin(kWeightControlW, header.getWidth() / 2);
    return header.removeFromRight(w).withSizeKeepingCentre(w, KhensuTheme::buttonHSmall);
}

void MixCraftPanel::paint(juce::Graphics& g)
{
    // UX2-1: the same flat card as the verdict above it.
    auto card = getLocalBounds().toFloat().reduced(0.5f);
    g.setColour(KhensuTheme::bgSurface);
    g.fillRoundedRectangle(card, 8.0f);
    g.setColour(KhensuTheme::borderSubtle);
    g.drawRoundedRectangle(card, 8.0f, 1.0f);

    auto b = getLocalBounds().reduced(kPadX, kPadY);

    // Header: the title (the weighting control sits on its right, owned by
    // the screen - weightControlArea()).
    auto header = b.removeFromTop(kHeaderH);
    header.removeFromRight(juce::jmin(kWeightControlW, header.getWidth() / 2) + KhensuTheme::sp3);
    g.setFont(KhensuTheme::uiFont(KhensuTheme::kFontH1, juce::Font::bold));
    g.setColour(KhensuTheme::textPrimary);
    // OVH-1: the breakdown of the verdict's Readiness - no second big number.
    g.drawText(TRANS("Readiness breakdown"), header, juce::Justification::centredLeft, true);

    auto subtitle = b.removeFromTop(kSubtitleH);
    b.removeFromTop(KhensuTheme::sp3);
    g.setFont(KhensuTheme::labelFont(KhensuTheme::kFontLabel));
    g.setColour(KhensuTheme::textTertiary);
    if (!hasData_) {
        g.drawText(TRANS("What each part of the mix adds to the readiness score."), subtitle,
                   juce::Justification::centredLeft, true);
        g.setFont(KhensuTheme::bodyFont());
        g.setColour(KhensuTheme::textTertiary);
        g.drawText(TRANS("Load a track to see the category breakdown."), b.removeFromTop(kRowH),
                   juce::Justification::centredLeft, true);
        return;
    }

    const int total = static_cast<int>(scorecard_.categories.size());
    const int analyzedCount =
        static_cast<int>(std::round(scorecard_.completeness * static_cast<float>(total)));
    rowAreas_.clear();
    g.drawText(TRANS("Weighted score per category, 0 - 100") + "   " +
                   TRANS("%a of %t categories analysed")
                       .replace("%a", juce::String(analyzedCount))
                       .replace("%t", juce::String(total)),
               subtitle, juce::Justification::centredLeft, true);
    if (!spread_.empty()) { // VIZ-1c: what the bands behind the rows are
        g.setColour(KhensuTheme::textTertiary);
        g.drawText(TRANS("Bands: released") + " " + spreadName_ + " (" + juce::String(spreadTracks_) + ")", subtitle,
                   juce::Justification::centredRight, true);
    }

    const int nameW = juce::jlimit(150, 240, b.getWidth() * 34 / 100);

    // One row: a grade dot (only off target - severity colours mean
    // severity), the name and its weight, a bar to 100 with a tick at the
    // 80 "near target" line, and the score - the row's largest figure.
    auto drawRow = [&](juce::Rectangle<int> row, const juce::String& name, float weight, float score, bool available,
                       bool sub, const std::string& tag, const std::string& id) {
        if (!sub) rowAreas_.push_back({row, id});
        const auto nameFont = sub ? KhensuTheme::labelFont(KhensuTheme::kFontLabel) : KhensuTheme::bodyFont();
        auto nameArea = row.removeFromLeft(nameW);
        if (sub) nameArea.removeFromLeft(KhensuTheme::sp5);
        auto dotArea = nameArea.removeFromLeft(14);
        if (available && verdict::gradeOf(score) != verdict::Grade::Good) {
            g.setColour(verdict::colourFor(score));
            g.fillEllipse(juce::Rectangle<float>(sub ? 5.0f : 6.0f, sub ? 5.0f : 6.0f)
                              .withCentre(dotArea.toFloat().getCentre().withX((float)dotArea.getX() + 3.0f)));
        }
        const juce::String weightText = juce::String(juce::roundToInt(weight * 100.0f)) + "%";
        const auto weightFont = KhensuTheme::numericFont(KhensuTheme::kFontLabel);
        const int weightW = juce::GlyphArrangement::getStringWidthInt(weightFont, weightText) + 4;
        g.setFont(weightFont);
        g.setColour(KhensuTheme::textTertiary);
        g.drawText(weightText, nameArea.removeFromRight(weightW), juce::Justification::centredRight, false);
        g.setFont(nameFont);
        g.setColour(sub ? KhensuTheme::textSecondary : KhensuTheme::textPrimary);
        const int labelW = juce::jmin(nameArea.getWidth(),
                                      juce::GlyphArrangement::getStringWidthInt(nameFont, name) + 2);
        g.drawText(name, nameArea.removeFromLeft(labelW), juce::Justification::centredLeft, true);
        if (!tag.empty() && nameArea.getWidth() > 40) {
            // "Strongest" / "Improve": a quiet word, not a coloured chip.
            g.setFont(KhensuTheme::labelFont(KhensuTheme::kFontLabel));
            g.setColour(KhensuTheme::textTertiary);
            g.drawText(juce::String(tag).toLowerCase(), nameArea.withTrimmedLeft(KhensuTheme::sp2),
                       juce::Justification::centredLeft, true);
        }

        auto scoreArea = row.removeFromRight(kScoreW);
        row.removeFromLeft(KhensuTheme::sp4);
        row.removeFromRight(KhensuTheme::sp4);
        if (!available) {
            g.setFont(KhensuTheme::labelFont(KhensuTheme::kFontLabel));
            g.setColour(KhensuTheme::textTertiary);
            g.drawText(TRANS("Not yet analysed"), row.getUnion(scoreArea),
                       juce::Justification::centredRight, false);
            return;
        }
        const float barH = sub ? 4.0f : 6.0f;
        auto track = row.toFloat().withSizeKeepingCentre(static_cast<float>(row.getWidth()), barH);
        g.setColour(KhensuTheme::bgRaised);
        g.fillRoundedRectangle(track, barH * 0.5f);
        const float frac = juce::jlimit(0.0f, 1.0f, score / 100.0f);
        const auto spreadIt = sub ? spread_.end() : spread_.find(id);
        if (spreadIt != spread_.end()) {
            // VIZ-1c: released tracks' middle 80 % and 50 % and their median,
            // with this track as a marker on the same line.
            const auto& p = spreadIt->second;
            auto xAt = [&](float v) { return track.getX() + track.getWidth() * juce::jlimit(0.0f, 1.0f, v / 100.0f); };
            const auto band = track.withSizeKeepingCentre(track.getWidth(), 14.0f);
            auto span = [&](int lo, int hi) {
                return band.withX(xAt(p[(size_t)lo])).withWidth(juce::jmax(2.0f, xAt(p[(size_t)hi]) - xAt(p[(size_t)lo])));
            };
            g.setColour(KhensuTheme::vizSpreadOuter);
            g.fillRoundedRectangle(span(GenreSpread::kP10, GenreSpread::kP90), 3.0f);
            g.setColour(KhensuTheme::vizSpreadInner);
            g.fillRoundedRectangle(span(GenreSpread::kP25, GenreSpread::kP75), 3.0f);
            g.setColour(KhensuTheme::vizMedian);
            g.fillRect(juce::Rectangle<float>(xAt(p[GenreSpread::kP50]) - 0.75f, band.getY(), 1.5f, band.getHeight()));
            g.setColour(KhensuTheme::accentTeal.withAlpha(0.35f));
            g.fillRoundedRectangle(track.withWidth(juce::jmax(barH, track.getWidth() * frac)), barH * 0.5f);
            const auto dot = juce::Rectangle<float>(10.0f, 10.0f).withCentre({xAt(score), track.getCentreY()});
            g.setColour(KhensuTheme::vizTrack);
            g.fillEllipse(dot);
            g.setColour(KhensuTheme::accentTeal);
            g.drawEllipse(dot, 2.0f);
        } else if (frac > 0.0f) {
            g.setColour(sub ? KhensuTheme::accentTeal.withAlpha(0.55f) : KhensuTheme::accentTeal);
            g.fillRoundedRectangle(track.withWidth(juce::jmax(barH, track.getWidth() * frac)), barH * 0.5f);
        }
        // The "near target" line (80): a hairline across the track.
        const float tickX = track.getX() + track.getWidth() * 0.8f;
        g.setColour(KhensuTheme::textTertiary.withAlpha(0.7f));
        g.drawVerticalLine(juce::roundToInt(tickX), track.getY() - 3.0f, track.getBottom() + 3.0f);

        g.setFont(KhensuTheme::numericFont(sub ? KhensuTheme::kFontBody : KhensuTheme::kFontH1,
                                           sub ? juce::Font::plain : juce::Font::bold));
        g.setColour(sub ? KhensuTheme::textSecondary : KhensuTheme::textPrimary);
        g.drawText(juce::String(juce::roundToInt(score)), scoreArea, juce::Justification::centredRight, false);
    };

    for (const auto& cat : scorecard_.categories) {
        drawRow(b.removeFromTop(kRowH), juce::String(cat.name), cat.weight, cat.score, cat.available, false, cat.tag,
                cat.id);
        // Technical Integrity's Clipping / Noise & Artefacts, indented.
        for (const auto& sub : cat.sub_categories)
            drawRow(b.removeFromTop(kSubRowH), juce::String(sub.name), sub.weight, sub.score, sub.available, true, {},
                    sub.id);
    }
}

} // namespace khensu
