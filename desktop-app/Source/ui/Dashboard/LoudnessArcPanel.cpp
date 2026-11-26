// desktop-app/Source/ui/Dashboard/LoudnessArcPanel.cpp
#include "LoudnessArcPanel.h"
#include <khensu/session/Session.h>
#include <khensu/targets/GenreDatabase.h>
#include "../../IAppController.h"
#include "../shared/KhensuLookAndFeel.h"
#include <algorithm>
#include <cmath>
#include "../shared/KhensuTheme.h"

namespace khensu {

namespace chart = ui::chart;

LoudnessArcPanel::LoudnessArcPanel(IAppController& c) : PanelCard(c, "Loudness Arc")
{
    setExplainer(TRANS("Loudness Arc"),
                 TRANS("Per-section integrated LUFS displayed as a horizontal bar chart. Shows the loudness "
                 "journey across your arrangement - chorus lift, verse headroom, and bridge dynamics. "
                 "The shaded band is the genre's acceptable LUFS range; the dashed line is its target. "
                 "Click a section to play from its start.\n\n"
                 "The footer scores your macrodynamics: the measured verse->chorus lift against what "
                 "this genre typically delivers. A flat arc usually means bus over-compression has "
                 "squashed the contrast your arrangement wrote - the chorus stops feeling like a lift "
                 "even though the parts changed."),
                 "integrated_loudness");
    chart_.setTitle(TRANS("Loudness per section"));
    chart_.setHorizontal(true);
    chart_.x = chart::scales::lufs();
    chart_.onBarClicked = [this](int i) {
        if (i >= 0 && i < static_cast<int>(bars_.size()))
            chart_transport::playFrom(controller_.playback(), bars_[static_cast<size_t>(i)].startSec);
    };
    chart_.setMouseCursor(juce::MouseCursor::PointingHandCursor);
    chart_.setTooltip(TRANS("Click a section to play from its start"));
    addChildComponent(chart_);
}

void LoudnessArcPanel::Chart::paintData(juce::Graphics& g, juce::Rectangle<float> plot)
{
    if (range) { // the genre's acceptable range, behind the bars
        const float x0 = px(range->first), x1 = px(range->second);
        g.setColour(KhensuTheme::vizSpreadInner);
        g.fillRect(juce::Rectangle<float>(std::min(x0, x1), plot.getY(), std::abs(x1 - x0), plot.getHeight()));
    }
    ThemedBarChart::paintData(g, plot);
    if (target) {
        juce::Path line;
        line.startNewSubPath(px(*target), plot.getY());
        line.lineTo(px(*target), plot.getBottom());
        juce::Path dashed;
        const float dashes[] = {3.0f, 3.0f};
        juce::PathStrokeType(1.2f).createDashedStroke(dashed, line, dashes, 2);
        g.setColour(KhensuTheme::vizMedian);
        g.fillPath(dashed); // createDashedStroke returns a filled outline
    }
}

void LoudnessArcPanel::setData(const AnalysisResult& r)
{
    bars_.clear();
    // Only measured sections: no section analysis means no arc (the empty
    // state says so) - never a made-up example arc.
    if (r.section_analysis.has_value())
        for (const auto& s : r.section_analysis->sections) {
            SectionBar b;
            b.label = juce::String(s.label);
            b.lufs = s.lufs_integrated;
            b.startSec = s.start_sec;
            b.valid = (s.lufs_integrated < -0.5f); // 0.0f = not computed
            bars_.push_back(b);
        }
    computeContrast();
    rebuildChart();
    resized(); // the footer row comes and goes with the contrast
    repaint();
}

void LoudnessArcPanel::rebuildChart()
{
    chart_.setVisible(!bars_.empty());
    double lo = chart::scales::kLufsHi, hi = chart::scales::kLufsLo;
    std::vector<chart::Bar> bars;
    for (const auto& b : bars_) {
        chart::Bar bar;
        bar.label = b.label;
        bar.values = {b.valid ? static_cast<double>(b.lufs) : std::nan("")};
        if (b.valid) {
            lo = std::min(lo, static_cast<double>(b.lufs));
            hi = std::max(hi, static_cast<double>(b.lufs));
        } else {
            bar.note = TRANS("not measured");
        }
        bars.push_back(bar);
    }
    chart_.range.reset();
    chart_.target.reset();
    juce::String genre;
    if (auto* session = controller_.getActiveSession())
        if (auto g = controller_.genreDatabase().findById(session->target.primary_genre)) {
            chart_.range = std::make_pair(static_cast<double>(g->lufs_range_min), static_cast<double>(g->lufs_range_max));
            chart_.target = g->lufs_target;
            lo = std::min(lo, static_cast<double>(g->lufs_range_min));
            hi = std::max(hi, static_cast<double>(g->lufs_range_max));
            genre = juce::String(g->name);
        }
    // VIS-3: the standard loudness axis, narrowed to what is shown.
    chart_.x = chart::scales::autoFit(chart::scales::lufs(), lo, hi, 3.0, 12.0);
    chart_.setXRange(chart_.x.lo, chart_.x.hi);
    // The bars grow from the left edge; their length is loudness.
    chart_.setBaseline(chart_.x.lo);
    chart_.setBars(std::move(bars), {{TRANS("Integrated"), KhensuTheme::vizCategory(0)}});
    chart_.valueText = [](size_t, size_t, double v) { return juce::String(v, 1) + " LUFS"; };

    // VIS-8: the arc in words.
    juce::StringArray parts;
    for (const auto& b : bars_)
        parts.add(b.label + " " + (b.valid ? juce::String(b.lufs, 1) + " LUFS" : TRANS("not measured")));
    juce::String summary = TRANS("Loudness per section") + ": " + parts.joinIntoString(", ");
    if (chart_.range)
        summary << ". " << (genre.isNotEmpty() ? genre + " " : juce::String()) << TRANS("range") << " "
                << juce::String(chart_.range->first, 1) << " " << TRANS("to") << " " << juce::String(chart_.range->second, 1)
                << " LUFS";
    if (contrastValid_)
        summary << ". " << TRANS("Lift") << " " << juce::String(measuredContrastDb_, 1) << " dB, "
                << TRANS("the genre expects about") << " " << juce::String(expectedContrastDb_, 1) << " dB";
    chart_.setSummary(summary + ".");
}

void LoudnessArcPanel::computeContrast()
{
    contrastValid_ = false;

    // Lift sections vs. base sections: real role names when present, else the
    // roles inferred from the detector's repeated A/B/C clusters, else the
    // loudest section vs. the median (approximate) - see SectionContrast.h.
    std::vector<section_contrast::Bar> measured;
    for (const auto& bar : bars_)
        if (bar.valid) measured.push_back({bar.label, bar.lufs});
    contrast_ = section_contrast::compute(measured);
    if (contrast_.basis == section_contrast::Basis::None) return; // too few sections for a meaningful arc
    measuredContrastDb_ = contrast_.liftDb;

    // Map the genre's 0-1 chorus_contrast_norm onto a dB expectation. 0.0 ->
    // ~1 dB (through-composed / ambient: barely any lift), 0.5 -> ~5 dB
    // (typical pop lift), 1.0 -> ~9 dB (EDM drop-style contrast).
    expectedContrastDb_ = 1.0f;
    if (auto* session = controller_.getActiveSession())
        if (auto genre = controller_.genreDatabase().findById(session->target.primary_genre))
            expectedContrastDb_ = 1.0f + genre->chorus_contrast_norm * 8.0f;

    contrastValid_ = true;
}

void LoudnessArcPanel::resized()
{
    PanelCard::resized();
    auto b = bodyArea();
    if (contrastValid_) b.removeFromBottom(18);
    chart_.setBounds(b);
}

void LoudnessArcPanel::paint(juce::Graphics& g)
{
    PanelCard::paint(g);
    if (bars_.empty()) {
        drawEmptyState(g, "No loudness arc yet", "Run a full analysis to see the per-section loudness arc.");
        return;
    }

    // ---- Macrodynamics contrast footer ----
    if (!contrastValid_) return;
    const auto contrastRow = bodyArea().removeFromBottom(16);
    const float delta = measuredContrastDb_ - expectedContrastDb_;
    // Within +/-2 dB of the genre expectation = healthy; short = flattened
    // (the over-compression signature); far over = whiplash contrast. The
    // words carry the verdict; colour only marks a problem (VIS-4).
    const juce::Colour col = std::abs(delta) <= 2.0f ? KhensuTheme::textSecondary
                             : delta < 0.0f          ? KhensuTheme::accentRed
                                                     : KhensuTheme::accentAmber;
    const juce::String verdict = std::abs(delta) <= 2.0f ? "healthy contrast"
                                 : delta < 0.0f          ? "flattened - check bus compression/limiting"
                                                         : "extreme - verses may feel disconnected";

    juce::String text;
    using section_contrast::Basis;
    if (contrast_.basis == Basis::RoleLabels)
        text << "Verse->chorus lift: ";
    else if (contrast_.basis == Basis::RepeatStructure)
        // e.g. "Repeated-section lift (B over A, inferred): " - roles come
        // from which repeating section is loudest, not from named parts.
        text << TRANS("Repeated-section lift") << " (" << contrast_.liftLabel << " " << TRANS("over") << " "
             << contrast_.baseLabel << ", " << TRANS("inferred") << "): ";
    else
        text << "Peak-section lift (approx): ";
    text << juce::String(measuredContrastDb_, 1) << " dB  -  genre expects ~" << juce::String(expectedContrastDb_, 1)
         << " dB  -  " << verdict;

    g.setFont(KhensuTheme::labelFont(KhensuTheme::kFontLabel));
    g.setColour(col);
    g.drawText(text, contrastRow, juce::Justification::centredLeft, true);
}

} // namespace khensu
