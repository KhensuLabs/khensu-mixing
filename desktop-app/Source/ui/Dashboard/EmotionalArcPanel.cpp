// desktop-app/Source/ui/Dashboard/EmotionalArcPanel.cpp
#include "EmotionalArcPanel.h"
#include "../shared/KhensuLookAndFeel.h"
#include "../shared/KhensuTheme.h"

namespace khensu {

EmotionalArcPanel::EmotionalArcPanel(IAppController& c) : PanelCard(c, "Emotional Arc")
{
    setExplainer(TRANS("Emotional Arc"), TRANS("Energy and mood measured in 8 windows across the song's runtime instead of once for "
                                  "the whole track - shows how the track's intensity actually moves (or fails to). "
                                  "Dashed-outline sections mark runs where energy barely changes across 2+ consecutive "
                                  "windows, a candidate for \"this section reads as static.\""));
}

juce::Colour EmotionalArcPanel::colourForMood(const juce::String& mood)
{
    // UX2-3: a mood is not a severity - one accent at falling strength,
    // the label beside each band names it.
    if (mood == "euphoric") return KhensuTheme::accentTeal;
    if (mood == "uplifting") return KhensuTheme::accentTeal.withAlpha(0.7f);
    if (mood == "intense" || mood == "aggressive") return KhensuTheme::textPrimary; // "aggressive": older cached results
    if (mood == "melancholic") return KhensuTheme::textSecondary;
    return KhensuTheme::textTertiary;
}

void EmotionalArcPanel::setData(const AnalysisResult& r)
{
    // Only a measured arc is drawn. An invented 8-window arc used to stand in
    // whenever the deep pass hadn't produced one - after a real analysis too.
    hasData_ = r.emotional_arc.has_value();
    analysisExists_ = r.has_fast_analysis();
    arc_ = hasData_ ? *r.emotional_arc : AnalysisResult::EmotionalArcData{};
    repaint();
}

void EmotionalArcPanel::paint(juce::Graphics& g)
{
    PanelCard::paint(g);
    hover_.clear();
    auto b = bodyArea().toFloat();

    if (!hasData_ || arc_.points.size() < 2) {
        if (analysisExists_)
            drawEmptyState(g, TRANS("Energy/mood arc needs deep analysis"),
                           TRANS("It appears once the deep (service) pass completes - re-run it from the header if it "
                                 "failed."));
        else
            drawEmptyState(g, TRANS("No energy/mood arc yet"), TRANS("Run a full analysis to see the energy and mood arc."));
        return;
    }

    // Reserve a strip at the bottom for mood colour bands + labels.
    auto moodStripArea = b.removeFromBottom(24.0f);
    b.removeFromBottom(4.0f);

    // Flat sections drawn first, underneath the line, as dashed-outline boxes.
    g.setColour(KhensuTheme::textSecondary.withAlpha(0.10f));
    for (const auto& [startFrac, endFrac] : arc_.flat_sections) {
        auto rect = juce::Rectangle<float>(b.getX() + startFrac * b.getWidth(), b.getY(),
                                           (endFrac - startFrac) * b.getWidth(), b.getHeight());
        g.fillRect(rect);
        g.setColour(KhensuTheme::textSecondary.withAlpha(0.5f));
        float dashLengths[] = {4.0f, 3.0f};
        g.drawDashedLine(juce::Line<float>(rect.getTopLeft(), rect.getTopRight()), dashLengths, 2, 1.0f);
        g.drawDashedLine(juce::Line<float>(rect.getBottomLeft(), rect.getBottomRight()), dashLengths, 2, 1.0f);
        g.setColour(KhensuTheme::textSecondary.withAlpha(0.10f));
    }

    // Energy line (0-100 -> bottom..top of the plot area).
    juce::Path path;
    for (size_t i = 0; i < arc_.points.size(); ++i) {
        const auto& p = arc_.points[i];
        const float x = b.getX() + p.progress_fraction * b.getWidth();
        const float y = b.getBottom() - (p.energy_0_100 / 100.0f) * b.getHeight();
        if (i == 0)
            path.startNewSubPath(x, y);
        else
            path.lineTo(x, y);
    }
    g.setColour(KhensuTheme::accentTeal);
    g.strokePath(path, juce::PathStrokeType(2.0f));

    for (const auto& p : arc_.points) {
        const float x = b.getX() + p.progress_fraction * b.getWidth();
        const float y = b.getBottom() - (p.energy_0_100 / 100.0f) * b.getHeight();
        g.setColour(colourForMood(p.mood));
        g.fillEllipse(x - 3.0f, y - 3.0f, 6.0f, 6.0f);
        hover_.addColumn(x - 8.0f, x + 8.0f, b,
                         juce::String(juce::roundToInt(p.progress_fraction * 100.0f)) + " % " + TRANS("through the song")
                             + ": " + TRANS("energy") + " " + juce::String(juce::roundToInt(p.energy_0_100)) + ", "
                             + TRANS("mood") + " " + juce::String(juce::CharPointer_UTF8(p.mood.c_str())));
    }

    // Mood colour band underneath: one segment per window, spanning to the
    // next window's start (last window spans to the end).
    for (size_t i = 0; i < arc_.points.size(); ++i) {
        const float x0 = moodStripArea.getX() + arc_.points[i].progress_fraction * moodStripArea.getWidth();
        const float x1 = (i + 1 < arc_.points.size())
                             ? moodStripArea.getX() + arc_.points[i + 1].progress_fraction * moodStripArea.getWidth()
                             : moodStripArea.getRight();
        g.setColour(colourForMood(arc_.points[i].mood).withAlpha(0.6f));
        g.fillRect(juce::Rectangle<float>(x0, moodStripArea.getY(), x1 - x0, moodStripArea.getHeight() - 12.0f));
    }

    g.setFont(KhensuTheme::labelFont(8.5f));
    g.setColour(KhensuTheme::textTertiary);
    // NEW-9: the whole-track mood from the CLAP probe (experimental), when it ran.
    juce::String legend = TRANS("Energy 0-100, colour = mood per window - dashed = flat section");
    if (!arc_.track_mood_tags.empty()) {
        juce::StringArray tags;
        for (const auto& t : arc_.track_mood_tags)
            tags.add(juce::String(t));
        legend << "  |  " << TRANS("Track mood (learned, experimental):") << " " << tags.joinIntoString(", ");
    }
    g.drawText(legend, moodStripArea.removeFromBottom(12.0f).toNearestInt(), juce::Justification::centredLeft,
               false);
}

void EmotionalArcPanel::resized() { PanelCard::resized(); }

} // namespace khensu
