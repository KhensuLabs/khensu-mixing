// desktop-app/Source/ui/shared/FixDifferenceView.cpp
#include "FixDifferenceView.h"
#include <khensu/ui/ChartScales.h>
#include <khensu/analysis/BandPower.h>
#include <khensu/ui/VizPalette.h>
#include <cmath>
#include <limits>

namespace khensu {

namespace fix_difference {

std::vector<double> longTermBandDb(const TrackTimeline& tl)
{
    std::vector<double> out;
    if (tl.spectrogramDb.empty()) return out;
    const size_t bands = tl.spectrogramDb.front().size();
    out.assign(bands, 0.0);
    for (const auto& row : tl.spectrogramDb)
        for (size_t b = 0; b < bands && b < row.size(); ++b)
            out[b] += std::pow(10.0, row[b] / 10.0);
    for (auto& v : out)
        v = 10.0 * std::log10(v / static_cast<double>(tl.spectrogramDb.size()) + 1e-12);
    return out;
}

Result compute(const TrackTimeline& original, const TrackTimeline& current)
{
    Result r;
    const auto a = longTermBandDb(original), b = longTermBandDb(current);
    if (a.empty() || b.empty() || original.bandHz.size() != a.size() || current.bandHz.size() != b.size()) return r;
    // Read the current version on the original's band grid (the sample rates
    // can differ after an export).
    std::vector<band_power::Point> cur;
    for (size_t i = 0; i < b.size(); ++i)
        cur.push_back({current.bandHz[i], static_cast<float>(b[i])});
    for (size_t i = 0; i < a.size(); ++i) {
        const float hz = original.bandHz[i];
        if (hz > current.bandHz.back() * 1.01f) break;
        r.hz.push_back(hz);
        r.spectrumDb.push_back(static_cast<double>(band_power::at(cur, hz)) - a[i]);
    }
    const double hop = original.hopSec;
    const size_t n = std::min(original.shortTermLufs.size(), static_cast<size_t>(current.durationSec / hop));
    for (size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) * hop;
        const float o = original.shortTermLufs[i];
        const float c = TrackTimeline::at(current.shortTermLufs, current.hopSec, t);
        r.timeSec.push_back(t);
        r.loudnessDb.push_back(o > -60.0f && c > -60.0f ? static_cast<double>(c - o)
                                                        : std::numeric_limits<double>::quiet_NaN());
    }
    return r;
}

} // namespace fix_difference

FixDifferenceView::FixDifferenceView()
{
    setTitle(TRANS("What the current version changed"));
    spectrum_.x = ui::chart::scales::frequency(); // VIS-3: the one frequency axis
    spectrum_.setXRange(spectrum_.x.lo, spectrum_.x.hi);
    spectrum_.y.unit = "dB";
    spectrum_.setTitle(TRANS("Difference spectrum"));
    spectrum_.setTooltip(TRANS("The current version's spectrum minus the original's, measured. Red: energy added; blue: taken away."));
    loudness_.x.format = ui::chart::Format::Time;
    loudness_.y.unit = "LU";
    loudness_.setZoomable(true);
    loudness_.setTitle(TRANS("Difference loudness"));
    loudness_.setTooltip(TRANS("The current version's short-term loudness minus the original's, over time."));
    spectrum_.r = &result_;
    loudness_.r = &result_;
    addAndMakeVisible(spectrum_);
    addAndMakeVisible(loudness_);
}

void FixDifferenceView::setDifference(fix_difference::Result r, const juce::String& caption)
{
    result_ = std::move(r);
    caption_ = caption;
    auto range = [](const std::vector<double>& v, double floor) {
        double m = floor;
        for (const double x : v)
            if (std::isfinite(x)) m = std::max(m, std::abs(x));
        return std::ceil(m * 1.15);
    };
    const double sr = range(result_.spectrumDb, 3.0), lr = range(result_.loudnessDb, 2.0);
    spectrum_.y.lo = -sr;
    spectrum_.y.hi = sr;
    loudness_.y.lo = -lr;
    loudness_.y.hi = lr;
    loudness_.setXRange(0.0, result_.timeSec.empty() ? 1.0 : result_.timeSec.back());
    spectrum_.setLegend({{TRANS("Added"), juce::Colour(ui::viz::kDivergingWarm), ui::chart::LegendItem::Kind::Band},
                         {TRANS("Removed"), juce::Colour(ui::viz::kDivergingCool), ui::chart::LegendItem::Kind::Band}});
    repaint();
    spectrum_.repaint();
    loudness_.repaint();
}

void FixDifferenceView::clear()
{
    setDifference({}, {});
}

void FixDifferenceView::resized()
{
    auto b = getLocalBounds();
    b.removeFromTop(18);
    const int half = (b.getWidth() - KhensuTheme::sp3) / 2;
    spectrum_.setBounds(b.removeFromLeft(half));
    b.removeFromLeft(KhensuTheme::sp3);
    loudness_.setBounds(b);
}

void FixDifferenceView::paint(juce::Graphics& g)
{
    g.setColour(KhensuTheme::textTertiary);
    g.setFont(KhensuTheme::labelFont(KhensuTheme::kFontLabel));
    auto top = getLocalBounds().removeFromTop(16);
    g.drawText(result_.empty() ? TRANS("Measuring what the current version changed...") : caption_, top,
               juce::Justification::centredLeft, true);
}

void FixDifferenceView::SpectrumChart::paintData(juce::Graphics& g, juce::Rectangle<float> plot)
{
    if (r == nullptr || r->empty()) return;
    const float y0 = py(0.0);
    // Diverging fill per band: warm above zero, cool below, strength by size.
    for (size_t i = 0; i + 1 < r->hz.size(); ++i) {
        const double d = 0.5 * (r->spectrumDb[i] + r->spectrumDb[i + 1]);
        const float x0 = px(r->hz[i]), x1 = px(r->hz[i + 1]);
        const float yv = py(d);
        g.setColour(KhensuTheme::vizDiverging(static_cast<float>(juce::jlimit(-1.0, 1.0, d / std::max(1.0, y.hi)))).withAlpha(0.85f));
        g.fillRect(juce::Rectangle<float>(x0, std::min(y0, yv), std::max(1.0f, x1 - x0), std::abs(yv - y0)));
    }
    g.setColour(KhensuTheme::vizAxis);
    g.drawHorizontalLine(juce::roundToInt(y0), plot.getX(), plot.getRight());
    g.setColour(KhensuTheme::textPrimary);
    g.strokePath(seriesPath(r->hz, r->spectrumDb), juce::PathStrokeType(1.4f));
}

ui::chart::Readout FixDifferenceView::SpectrumChart::readoutAt(double xv, double)
{
    ui::chart::Readout out;
    out.title = x.label(xv, true);
    if (r == nullptr || r->empty()) return out;
    std::vector<band_power::Point> pts;
    for (size_t i = 0; i < r->hz.size(); ++i)
        pts.push_back({static_cast<float>(r->hz[i]), static_cast<float>(r->spectrumDb[i])});
    const float d = band_power::at(pts, static_cast<float>(xv));
    out.lines.push_back({(d >= 0.0f ? "+" : "") + juce::String(d, 1) + " dB " + (d >= 0.0f ? TRANS("added") : TRANS("removed")),
                         KhensuTheme::vizDiverging(d >= 0.0f ? 1.0f : -1.0f)});
    return out;
}

void FixDifferenceView::LoudnessChart::paintData(juce::Graphics& g, juce::Rectangle<float> plot)
{
    if (r == nullptr || r->empty()) return;
    g.setColour(KhensuTheme::vizAxis);
    g.drawHorizontalLine(juce::roundToInt(py(0.0)), plot.getX(), plot.getRight());
    g.setColour(KhensuTheme::vizCategory(0));
    g.strokePath(seriesPath(r->timeSec, r->loudnessDb), juce::PathStrokeType(1.6f));
}

ui::chart::Readout FixDifferenceView::LoudnessChart::readoutAt(double xv, double)
{
    ui::chart::Readout out;
    out.title = x.label(xv, true);
    if (r == nullptr || r->timeSec.empty()) return out;
    const double hop = r->timeSec.size() > 1 ? r->timeSec[1] - r->timeSec[0] : 0.1;
    const auto i = static_cast<size_t>(std::clamp(xv / hop, 0.0, static_cast<double>(r->timeSec.size() - 1)));
    const double d = r->loudnessDb[i];
    out.lines.push_back({std::isfinite(d) ? (d >= 0.0 ? "+" : "") + juce::String(d, 1) + " LU " + TRANS("vs the original")
                                          : TRANS("Silence"),
                         KhensuTheme::vizCategory(0)});
    return out;
}

} // namespace khensu
