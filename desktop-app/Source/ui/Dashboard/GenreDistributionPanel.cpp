// desktop-app/Source/ui/Dashboard/GenreDistributionPanel.cpp
#include "GenreDistributionPanel.h"
#include "../shared/PanelRunState.h"
#include "../shared/SessionEpoch.h"
#include "../shared/KhensuLookAndFeel.h"
#include "../shared/ServiceErrorHint.h"
#include <khensu/ipc/HttpServiceBridge.h>
#include <algorithm>

namespace khensu {

GenreDistributionPanel::GenreDistributionPanel(IAppController& c) : PanelCard(c, "Loudness vs Genre")
{
    setExplainer(TRANS("Loudness vs Genre"), TRANS("Where this track's loudness (LUFS), loudness range (LRA), and crest "
                                      "factor sit inside the genre's reference distribution - a box plot per "
                                      "metric (min / quartiles / max) with your value marked and its "
                                      "percentile. Directional only when the reference library is bootstrap/"
                                      "synthetic, per the safety audit's provenance rules."));
    setAnalysisState(PanelAnalysisState::NotAnalyzed);
}

void GenreDistributionPanel::setData(const AnalysisResult& r)
{
    trackLufs_ = r.loudness.lufs_integrated;
    trackLra_ = r.loudness.lra;
    trackCrest_ = r.dynamics.crest_factor_db;

    juce::String genre;
    if (r.benchmark.has_value()) {
        genre = juce::String(r.benchmark->genre_id);
        bootstrap_ = r.benchmark->is_bootstrap_data;
    }
    if (genre.isEmpty() || genre == genreId_) {
        if (genre.isEmpty()) {
            setAnalysisState(PanelAnalysisState::NotAnalyzed);
            repaint();
        } else
            repaint();
        return;
    }
    genreId_ = genre;
    loading_ = true;
    fetchError_.clear();
    setAnalysisState(PanelAnalysisState::Analyzing);

    juce::Component::SafePointer<GenreDistributionPanel> self(this);
    const std::string gid = genreId_.toStdString();
    controller_.runInBackground([self, &work = controller_, gid]() {
        ipc::HttpServiceBridge bridge;
        auto res = bridge.getReferenceLibraryPoints(gid);
        postIfCurrent(self, work, [res](GenreDistributionPanel& panel) {
            if (res.is_err()) {
                panel.fetchError_ = serviceErrorLine(res.error());
                panel.setAnalysisState(PanelAnalysisState::NotAnalyzed);
                panel.repaint();
                return;
            }
            panel.points_ = res.value();
            panel.setAnalysisState(PanelAnalysisState::Analyzed);
            panel.repaint();
        }, [](GenreDistributionPanel& panel) { panel.loading_ = false; });
    });
}

void GenreDistributionPanel::drawBox(juce::Graphics& g, juce::Rectangle<int> area, const juce::String& label,
                                     std::vector<float> values, float trackValue)
{
    g.setColour(KhensuTheme::textSecondary);
    g.setFont(KhensuTheme::labelFont(10.0f));
    auto labelArea = area.removeFromLeft(46);
    g.drawText(label, labelArea, juce::Justification::centredLeft, false);

    if (values.size() < 4) {
        g.setColour(KhensuTheme::textTertiary);
        g.drawText("(insufficient reference data)", area, juce::Justification::centredLeft, false);
        return;
    }
    std::sort(values.begin(), values.end());
    auto quant = [&](float q) {
        const float idx = q * (float)(values.size() - 1);
        const int lo = (int)idx;
        const int hi = std::min(lo + 1, (int)values.size() - 1);
        return values[(size_t)lo] + (idx - (float)lo) * (values[(size_t)hi] - values[(size_t)lo]);
    };
    const float mn = values.front(), mx = values.back();
    const float q1 = quant(0.25f), med = quant(0.5f), q3 = quant(0.75f);

    // Axis range with a little padding, including the track value.
    float axLo = std::min(mn, trackValue), axHi = std::max(mx, trackValue);
    const float pad = juce::jmax(0.5f, (axHi - axLo) * 0.08f);
    axLo -= pad;
    axHi += pad;
    auto xFor = [&](float v) {
        const float frac = (axHi > axLo) ? (v - axLo) / (axHi - axLo) : 0.5f;
        return (float)area.getX() + juce::jlimit(0.0f, 1.0f, frac) * (float)area.getWidth();
    };

    const float cy = (float)area.getCentreY();
    const float boxH = 10.0f;
    // Whiskers.
    g.setColour(KhensuTheme::textTertiary.withAlpha(0.7f));
    g.drawLine(xFor(mn), cy, xFor(q1), cy, 1.0f);
    g.drawLine(xFor(q3), cy, xFor(mx), cy, 1.0f);
    // Box (Q1-Q3).
    g.setColour(KhensuTheme::accentBlue.withAlpha(0.35f));
    g.fillRect(juce::Rectangle<float>(xFor(q1), cy - boxH / 2, xFor(q3) - xFor(q1), boxH));
    g.setColour(KhensuTheme::accentBlue);
    g.drawLine(xFor(med), cy - boxH / 2, xFor(med), cy + boxH / 2, 1.6f); // median

    // Track marker + percentile.
    const int below = (int)std::count_if(values.begin(), values.end(), [&](float v) { return v <= trackValue; });
    const int pct = (int)std::round(100.0f * (float)below / (float)values.size());
    const float tx = xFor(trackValue);
    g.setColour(KhensuTheme::accentTeal);
    g.fillEllipse(tx - 3.5f, cy - 3.5f, 7.0f, 7.0f);
    g.setFont(KhensuTheme::labelFont(9.0f));
    g.drawText(juce::String(trackValue, 1) + "  (" + juce::String(pct) + "%)", area.removeFromRight(90),
               juce::Justification::centredRight, false);
}

void GenreDistributionPanel::paint(juce::Graphics& g)
{
    PanelCard::paint(g);
    auto b = bodyArea();

    if (genreId_.isEmpty()) {
        drawEmptyState(g, "No genre benchmark yet", "Runs once a genre is set at intake.");
        return;
    }
    if (loading_) return; // PanelCard shows the analyzing chip
    if (fetchError_.isNotEmpty()) {
        drawEmptyState(g, "Couldn't load genre distribution", fetchError_);
        return;
    }
    if (points_.empty()) {
        drawEmptyState(g, "No reference distribution", "The genre library has no points for this genre.");
        return;
    }

    if (bootstrap_) {
        g.setColour(KhensuTheme::accentAmber.withAlpha(0.9f));
        g.setFont(KhensuTheme::labelFont(9.0f));
        g.drawText("Reference data is bootstrap/synthetic - percentiles are directional.", b.removeFromTop(14),
                   juce::Justification::centredLeft, false);
        b.removeFromTop(2);
    }

    std::vector<float> lufs, lra, crest;
    lufs.reserve(points_.size());
    lra.reserve(points_.size());
    crest.reserve(points_.size());
    for (const auto& p : points_) {
        lufs.push_back((float)p.lufs_integrated);
        lra.push_back((float)p.lra);
        crest.push_back((float)p.crest_factor_db);
    }

    const int rowH = juce::jmax(20, b.getHeight() / 3);
    drawBox(g, b.removeFromTop(rowH), "LUFS", lufs, trackLufs_);
    drawBox(g, b.removeFromTop(rowH), "LRA", lra, trackLra_);
    drawBox(g, b.removeFromTop(rowH), "Crest", crest, trackCrest_);
}

} // namespace khensu
