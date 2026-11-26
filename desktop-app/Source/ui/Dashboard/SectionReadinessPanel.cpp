// desktop-app/Source/ui/Dashboard/SectionReadinessPanel.cpp
#include "SectionReadinessPanel.h"
#include "../shared/KhensuLookAndFeel.h"
#include "../shared/KhensuTheme.h"

namespace khensu {

SectionReadinessPanel::SectionReadinessPanel(IAppController& c) : PanelCard(c, "Section Readiness")
{
    setExplainer(TRANS("Section Readiness"),
                 TRANS("A per-section proxy score (0-100) from each detected section's own "
                 "LUFS, crest factor, harshness, and sibilance -- so a dragging verse "
                 "or bridge shows up here instead of averaging away inside the single "
                 "whole-track Mastering Readiness score."),
                 "section_aware_analysis");
    setAnalysisState(PanelAnalysisState::NotAnalyzed);
}

float SectionReadinessPanel::computeScore(const SectionData& s)
{
    // Same style of banded scoring MixScorecard uses elsewhere: distance
    // from a plausible good-practice band, clamped 0..1, then averaged.
    auto bandScore = [](float v, float loGood, float hiGood, float loEdge, float hiEdge) {
        if (v >= loGood && v <= hiGood) return 1.0f;
        if (v < loGood) return juce::jlimit(0.0f, 1.0f, (v - loEdge) / (loGood - loEdge));
        return juce::jlimit(0.0f, 1.0f, (hiEdge - v) / (hiEdge - hiGood));
    };

    float total = 0.0f;
    int n = 0;

    if (s.lufs_integrated < 0.0f) { // 0.0f sentinel = not computed
        total += bandScore(s.lufs_integrated, -18.0f, -8.0f, -30.0f, -3.0f);
        ++n;
    }
    if (s.crest_db > 0.0f) {
        total += bandScore(s.crest_db, 6.0f, 18.0f, 2.0f, 24.0f);
        ++n;
    }
    if (s.harshness_score >= 0.0f) {
        total += juce::jlimit(0.0f, 1.0f, 1.0f - s.harshness_score);
        ++n;
    }
    if (s.sibilance_score >= 0.0f) {
        total += juce::jlimit(0.0f, 1.0f, 1.0f - s.sibilance_score);
        ++n;
    }

    return n > 0 ? total / (float)n : 0.5f;
}

void SectionReadinessPanel::setData(const AnalysisResult& r)
{
    sections_.clear();
    if (r.section_analysis.has_value()) {
        for (const auto& s : r.section_analysis->sections) {
            SectionScore sc;
            sc.label = juce::String(s.label);
            sc.startSec = s.start_sec;
            sc.endSec = s.end_sec;
            sc.score01 = computeScore(s);
            sections_.push_back(sc);
        }
    }
    setAnalysisState(r.section_analysis.has_value() ? PanelAnalysisState::Analyzed : PanelAnalysisState::NotAnalyzed);
    repaint();
}

void SectionReadinessPanel::resized() { PanelCard::resized(); }

void SectionReadinessPanel::paint(juce::Graphics& g)
{
    PanelCard::paint(g);
    auto body = bodyArea();

    if (sections_.empty()) {
        drawEmptyState(g, "No section data yet", "Run a full analysis to see per-section readiness.");
        return;
    }

    const float totalSec = sections_.back().endSec > 0.0f ? sections_.back().endSec : 1.0f;
    const float w = (float)body.getWidth();
    const float h = (float)body.getHeight() - 16.0f;

    for (const auto& sc : sections_) {
        const float x0 = body.getX() + (sc.startSec / totalSec) * w;
        const float x1 = body.getX() + (sc.endSec / totalSec) * w;
        const float barH = sc.score01 * h;

        const juce::Colour col = sc.score01 >= 0.66f   ? KhensuTheme::accentGreen
                                 : sc.score01 >= 0.33f ? KhensuTheme::accentAmber
                                                       : KhensuTheme::accentRed;
        g.setColour(col.withAlpha(0.8f));
        g.fillRect(x0 + 1.0f, (float)body.getY() + h - barH, juce::jmax(1.0f, x1 - x0 - 2.0f), barH);

        g.setColour(KhensuTheme::textSecondary.withAlpha(0.6f));
        g.setFont(khensu::KhensuTheme::uiFont(khensu::type::caption, juce::Font::plain));
        g.drawText(sc.label, (int)x0, body.getY() + (int)h + 2, (int)(x1 - x0), 14, juce::Justification::centred,
                   false);
    }

    g.setColour(KhensuTheme::textSecondary.withAlpha(0.2f));
    g.drawHorizontalLine(body.getY() + (int)h, (float)body.getX(), (float)body.getRight());
}

} // namespace khensu
