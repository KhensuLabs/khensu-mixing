// desktop-app/Source/ui/shared/StemViz.cpp
#include "StemViz.h"
#include <cmath>

namespace khensu {

namespace stem_viz {

Stream stream(const std::map<std::string, std::shared_ptr<const TrackTimeline>>& stems, double hopSec)
{
    Stream s;
    s.hopSec = hopSec;
    double duration = 0.0;
    for (const auto& [name, tl] : stems)
        if (tl != nullptr) duration = std::max(duration, tl->durationSec);
    if (duration <= 0.0) return s;
    for (double t = 0.0; t <= duration; t += hopSec)
        s.timeSec.push_back(t);
    std::vector<std::string> names;
    for (const auto& n : stemOrder())
        if (stems.count(n) != 0 && stems.at(n) != nullptr) names.push_back(n);
    for (const auto& [name, tl] : stems)
        if (tl != nullptr && std::find(names.begin(), names.end(), name) == names.end()) names.push_back(name);
    for (const auto& n : names) {
        const auto& tl = *stems.at(n);
        std::vector<float> e;
        e.reserve(s.timeSec.size());
        for (const double t : s.timeSec) {
            const float lufs = TrackTimeline::at(tl.shortTermLufs, tl.hopSec, t, -70.0f);
            e.push_back(lufs > -69.0f ? static_cast<float>(std::pow(10.0, (lufs + 0.691) / 10.0)) : 0.0f);
        }
        s.stems.push_back(n);
        s.energy.push_back(std::move(e));
    }
    return s;
}

MaskingMap masking(const std::string& a, const TrackTimeline& ta, const std::string& b, const TrackTimeline& tb)
{
    MaskingMap m;
    m.stemA = a;
    m.stemB = b;
    if (ta.spectrogramDb.empty() || tb.spectrogramDb.empty() || ta.bandHz.size() != tb.bandHz.size()) return m;
    m.bandHz = ta.bandHz;
    m.frameSec = ta.frameSec;
    const size_t bands = ta.bandHz.size();
    const double bandFloor = std::pow(10.0, -80.0 / 10.0), frameFloor = std::pow(10.0, -60.0 / 10.0);
    const double gateRel = std::pow(10.0, -20.0 / 10.0);
    for (size_t f = 0; f < ta.spectrogramDb.size(); ++f) {
        const double t = static_cast<double>(f) * ta.frameSec;
        const auto fb = static_cast<size_t>(std::clamp(tb.frameSec > 0.0 ? std::llround(t / tb.frameSec) : 0LL, 0LL,
                                                       static_cast<long long>(tb.spectrogramDb.size()) - 1));
        std::vector<double> ea(bands), eb(bands);
        double sa = 0.0, sb = 0.0;
        for (size_t k = 0; k < bands; ++k) {
            ea[k] = std::pow(10.0, ta.spectrogramDb[f][k] / 10.0);
            eb[k] = std::pow(10.0, tb.spectrogramDb[fb][k] / 10.0);
            sa += ea[k];
            sb += eb[k];
        }
        std::vector<float> row(bands, 0.0f);
        if (sa > frameFloor && sb > frameFloor)
            for (size_t k = 0; k < bands; ++k) {
                const double lo = std::min(ea[k], eb[k]), hi = std::max(ea[k], eb[k]);
                if (lo > bandFloor && lo >= hi * gateRel)
                    row[k] = static_cast<float>(std::min(ea[k] / sa, eb[k] / sb));
            }
        for (size_t k = 0; k < bands; ++k)
            if (row[k] > m.worst) {
                m.worst = row[k];
                m.worstSec = t;
                m.worstHz = m.bandHz[k];
            }
        m.grid.push_back(std::move(row));
    }
    if (m.worst > 0.0f)
        for (auto& row : m.grid)
            for (auto& v : row)
                v /= m.worst;
    return m;
}

} // namespace stem_viz

// ---- Streamgraph ------------------------------------------------------------------

StemStreamGraph::StemStreamGraph()
{
    x.format = ui::chart::Format::Time;
    y.lo = 0.0;
    y.hi = 1.0;
    setShowAxes(true, false);
    setMargins(8, 8, 8, 20);
    setZoomable(true);
    setTitle(TRANS("Stem energy over time"));
    setTooltip(TRANS("Each stem's energy over time, stacked around the centre line: the arrangement as shapes."));
}

void StemStreamGraph::setStream(stem_viz::Stream s)
{
    stream_ = std::move(s);
    maxTotal_ = 1e-12f;
    for (size_t f = 0; f < stream_.timeSec.size(); ++f) {
        float total = 0.0f;
        for (const auto& e : stream_.energy)
            total += std::sqrt(e[f]); // amplitude scale reads better than power
        maxTotal_ = std::max(maxTotal_, total);
    }
    setXRange(0.0, stream_.timeSec.empty() ? 1.0 : stream_.timeSec.back());
    std::vector<ui::chart::LegendItem> legend;
    for (const auto& n : stream_.stems)
        legend.push_back({juce::String(n), KhensuTheme::vizCategory(stem_viz::slotOf(n)), ui::chart::LegendItem::Kind::Band});
    setLegend(std::move(legend));
}

void StemStreamGraph::paintData(juce::Graphics& g, juce::Rectangle<float> plot)
{
    juce::ignoreUnused(plot);
    const size_t n = stream_.timeSec.size();
    if (n < 2 || stream_.energy.empty()) return;
    // Silhouette layout: the stack is centred on y = 0.5.
    std::vector<double> base(n);
    for (size_t f = 0; f < n; ++f) {
        double total = 0.0;
        for (const auto& e : stream_.energy)
            total += std::sqrt(e[f]);
        base[f] = 0.5 - 0.5 * total / maxTotal_;
    }
    for (size_t si = 0; si < stream_.energy.size(); ++si) {
        std::vector<double> lo(n), hi(n);
        for (size_t f = 0; f < n; ++f) {
            lo[f] = base[f];
            hi[f] = base[f] + std::sqrt(stream_.energy[si][f]) / maxTotal_;
            base[f] = hi[f];
        }
        const auto c = KhensuTheme::vizCategory(stem_viz::slotOf(stream_.stems[si]));
        g.setColour(c.withAlpha(0.85f));
        g.fillPath(bandPath(stream_.timeSec, lo, hi));
        g.setColour(KhensuTheme::bgSurface.withAlpha(0.6f)); // the 2 px surface gap between layers
        g.strokePath(seriesPath(stream_.timeSec, hi), juce::PathStrokeType(1.0f));
    }
}

ui::chart::Readout StemStreamGraph::readoutAt(double xv, double)
{
    ui::chart::Readout r;
    r.title = x.label(xv, true);
    if (stream_.timeSec.empty()) return r;
    const auto f = static_cast<size_t>(std::clamp(xv / stream_.hopSec, 0.0, static_cast<double>(stream_.timeSec.size() - 1)));
    double total = 0.0;
    for (const auto& e : stream_.energy)
        total += e[f];
    for (size_t si = 0; si < stream_.energy.size(); ++si) {
        const double e = stream_.energy[si][f];
        const juce::String level = e > 0.0 ? juce::String(10.0 * std::log10(e) - 0.691, 1) + " LUFS" : juce::String("-");
        const juce::String share = total > 0.0 ? juce::String(juce::roundToInt(100.0 * e / total)) + " %" : juce::String("-");
        r.lines.push_back({juce::String(stream_.stems[si]) + "  " + level + "  (" + share + ")",
                           KhensuTheme::vizCategory(stem_viz::slotOf(stream_.stems[si]))});
    }
    return r;
}

// ---- Masking heat map -------------------------------------------------------------

MaskingHeatmapView::MaskingHeatmapView()
{
    x.format = ui::chart::Format::Time;
    y.scale = ui::chart::Scale::Log;
    y.format = ui::chart::Format::Hz;
    y.lo = 30.0;
    y.hi = 16000.0;
    setZoomable(true);
    setMargins(40, 8, 8, 20);
    setTitle(TRANS("Masking map"));
    setTooltip(TRANS("Where the two stems put comparable energy in the same band at the same moment: the brighter, "
                     "the more they fight for that space."));
}

void MaskingHeatmapView::setMap(stem_viz::MaskingMap m)
{
    map_ = std::move(m);
    image_ = {};
    if (!map_.grid.empty() && !map_.bandHz.empty()) {
        y.lo = map_.bandHz.front();
        y.hi = map_.bandHz.back();
        const int frames = static_cast<int>(map_.grid.size()), bands = static_cast<int>(map_.bandHz.size());
        image_ = juce::Image(juce::Image::RGB, frames, bands, false);
        juce::Image::BitmapData px(image_, juce::Image::BitmapData::writeOnly);
        for (int f = 0; f < frames; ++f)
            for (int b = 0; b < bands; ++b)
                px.setPixelColour(f, bands - 1 - b, KhensuTheme::vizSequential(map_.grid[(size_t)f][(size_t)b]));
    }
    setXRange(0.0, std::max(1.0, map_.frameSec * static_cast<double>(map_.grid.size())));
}

void MaskingHeatmapView::paintData(juce::Graphics& g, juce::Rectangle<float> plot)
{
    if (!image_.isValid()) return;
    const double total = map_.frameSec * static_cast<double>(map_.grid.size());
    const float w = static_cast<float>(image_.getWidth());
    const int sx = juce::roundToInt(static_cast<float>(x.lo / total) * w);
    const int sw = std::max(1, juce::roundToInt(static_cast<float>((x.hi - x.lo) / total) * w));
    const auto dst = plot.toNearestInt();
    g.setImageResamplingQuality(juce::Graphics::lowResamplingQuality);
    g.drawImage(image_, dst.getX(), dst.getY(), dst.getWidth(), dst.getHeight(), sx, 0, sw, image_.getHeight());
}

ui::chart::Readout MaskingHeatmapView::readoutAt(double xv, double yv)
{
    ui::chart::Readout r;
    r.title = x.label(xv, true) + "  " + y.label(yv, true);
    if (map_.grid.empty() || map_.frameSec <= 0.0) return r;
    const auto f = static_cast<size_t>(std::clamp(xv / map_.frameSec, 0.0, static_cast<double>(map_.grid.size() - 1)));
    size_t b = 0;
    for (size_t k = 1; k < map_.bandHz.size(); ++k)
        if (std::abs(std::log(map_.bandHz[k] / yv)) < std::abs(std::log(map_.bandHz[b] / yv))) b = k;
    const float v = map_.grid[f][b];
    r.lines.push_back({juce::String(map_.stemA) + " / " + juce::String(map_.stemB) + ": "
                           + juce::String(juce::roundToInt(v * 100.0f)) + " % " + TRANS("of the worst clash"),
                       KhensuTheme::vizSequential(v)});
    return r;
}

} // namespace khensu
