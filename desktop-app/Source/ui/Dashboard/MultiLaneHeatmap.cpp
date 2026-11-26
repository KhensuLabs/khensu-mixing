// desktop-app/Source/ui/Dashboard/MultiLaneHeatmap.cpp
#include "MultiLaneHeatmap.h"
#include <khensu/ui/ChartScales.h>
#include "../shared/KhensuEmptyState.h"
#include "../shared/SpectrogramImage.h"
#include "../shared/VizMaps.h"
#include <khensu/ui/VizFrequencyAxis.h>
#include "../../services/IPlayback.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace khensu {

namespace {

/// The severity colour of a finding marker (red / amber / blue: severity,
/// never a data colour).
juce::Colour severityColour(float severity)
{
    return severity >= 66.0f ? KhensuTheme::accentRed : severity >= 33.0f ? KhensuTheme::accentAmber : KhensuTheme::accentBlue;
}

/// A non-zero count's colour on the sequential map: the lowest step is kept
/// off the background so a single finding still shows.
juce::Colour densityColour(float frac) { return KhensuTheme::vizSequential(0.2f + 0.8f * juce::jlimit(0.0f, 1.0f, frac)); }

/// The value of a data lane nearest `sec` (NaN when the lane has none).
float nearestValue(const TimelineDataLane& dl, double sec)
{
    float best = std::numeric_limits<float>::quiet_NaN();
    double bestDt = 1e12;
    for (const auto& s : dl.samples) {
        if (!std::isfinite(s.y)) continue;
        const double dt = std::abs(static_cast<double>(s.x) - sec);
        if (dt < bestDt) {
            bestDt = dt;
            best = s.y;
        }
    }
    return best;
}

/// The value range a lane's samples span (lo > hi when none are finite).
std::pair<float, float> dataRange(const TimelineDataLane& dl)
{
    float lo = std::numeric_limits<float>::max(), hi = std::numeric_limits<float>::lowest();
    for (const auto& s : dl.samples)
        if (std::isfinite(s.y)) {
            lo = std::min(lo, s.y);
            hi = std::max(hi, s.y);
        }
    return {lo, hi};
}

juce::String formatValue(float v, const juce::String& suffix)
{
    return juce::String(v, suffix.isEmpty() ? 2 : 1) + suffix;
}

} // namespace

MultiLaneHeatmap::MultiLaneHeatmap(IPlayback* playback) : playback_(playback)
{
    x = ui::chart::scales::time(1.0);
    y = ui::chart::scales::categories({}, true);
    setShowAxes(true, true);
    setGridLines(true, false); // time lines only; the rows draw their own separators
    setMargins(68, 6, 10, 20);
    setZoomable(false); // the card scrolls with the Dashboard - the wheel is not ours
    setTitle(TRANS("Issue heatmap"));
    densityCounts_.assign(kDensityBuckets, 0);
    if (playback_ != nullptr) {
        chart_transport::attachClickToPlay(*this, *playback_);
        playheadFollower_ = std::make_unique<chart_transport::PlayheadFollower>(*this, *playback_);
    }
}

MultiLaneHeatmap::~MultiLaneHeatmap() { playheadFollower_.reset(); }

void MultiLaneHeatmap::setSpan(float t)
{
    total_ = std::max(0.0f, t);
    x = ui::chart::scales::time(total_ > 0.0f ? total_ : 1.0);
    setXRange(x.lo, x.hi);
    rebuildDensity();
    repaint();
}

void MultiLaneHeatmap::setLanes(std::vector<HeatmapLaneData> lanes)
{
    lanes_ = std::move(lanes);
    rebuildDensity();
    rebuildRows();
}

void MultiLaneHeatmap::setDensityEnabled(bool enabled)
{
    densityEnabled_ = enabled;
    rebuildDensity();
    rebuildRows();
}

void MultiLaneHeatmap::setDataLanes(std::vector<TimelineDataLane> dataLanes)
{
    dataLanes_ = std::move(dataLanes);
    rebuildRows();
}

void MultiLaneHeatmap::setSpectrogram(std::vector<std::vector<float>> frames)
{
    spectrogramFrames_ = std::move(frames);
    spectrogramImageDirty_ = true;
    rebuildRows();
}

bool MultiLaneHeatmap::isEmpty() const noexcept { return total_ <= 0.0f || (lanes_.empty() && dataLanes_.empty()); }

void MultiLaneHeatmap::rebuildRows()
{
    rows_.clear();
    if (!spectrogramFrames_.empty() && !spectrogramFrames_.front().empty())
        rows_.push_back({RowKind::Spectrum, 0, TRANS("Spectrum")});
    if (densityEnabled_) rows_.push_back({RowKind::Density, 0, TRANS("Density")});
    for (int i = 0; i < static_cast<int>(dataLanes_.size()); ++i)
        rows_.push_back({RowKind::Data, i, dataLanes_[static_cast<size_t>(i)].label});
    for (int i = 0; i < static_cast<int>(lanes_.size()); ++i)
        rows_.push_back({RowKind::Marker, i, lanes_[static_cast<size_t>(i)].label});

    std::vector<juce::String> names;
    names.reserve(rows_.size());
    for (const auto& r : rows_)
        names.push_back(r.name);
    y = ui::chart::scales::categories(std::move(names), true);
    if (auto* h = getAccessibilityHandler()) h->notifyAccessibilityEvent(juce::AccessibilityEvent::valueChanged);
    repaint();
}

void MultiLaneHeatmap::rebuildDensity()
{
    densityCounts_.assign(kDensityBuckets, 0);
    densityMax_ = 0;
    if (total_ <= 0.0f) return;
    for (const auto& lane : lanes_)
        for (const auto& rec : lane.recs) {
            if (!rec.time_range.has_value()) continue;
            const float t = rec.time_range->start_sec;
            const int bucket = juce::jlimit(0, kDensityBuckets - 1, static_cast<int>((t / total_) * static_cast<float>(kDensityBuckets)));
            densityMax_ = std::max(densityMax_, ++densityCounts_[static_cast<size_t>(bucket)]);
        }
}

juce::String MultiLaneHeatmap::rowName(int row) const
{
    return row >= 0 && row < rowCount() ? rows_[static_cast<size_t>(row)].name : juce::String();
}

juce::Rectangle<float> MultiLaneHeatmap::rowArea(int row) const
{
    const auto plot = plotArea();
    if (row < 0 || row >= rowCount() || plot.isEmpty()) return {};
    const float top = py(row - 0.5), bottom = py(row + 0.5);
    return {plot.getX(), std::min(top, bottom), plot.getWidth(), std::abs(bottom - top)};
}

int MultiLaneHeatmap::rowAt(double yValue) const
{
    if (rows_.empty()) return -1;
    return juce::jlimit(0, rowCount() - 1, juce::roundToInt(yValue));
}

void MultiLaneHeatmap::paint(juce::Graphics& g)
{
    if (isEmpty()) {
        KhensuEmptyState::paintInto(g, getLocalBounds(), TRANS("No timeline data"), {}); // UXR-6
        return;
    }
    ThemedChart::paint(g);
}

void MultiLaneHeatmap::paintData(juce::Graphics& g, juce::Rectangle<float> plot)
{
    juce::ignoreUnused(plot);
    for (int i = 0; i < rowCount(); ++i) {
        const auto area = rowArea(i);
        const auto& row = rows_[static_cast<size_t>(i)];
        g.setColour((i % 2 == 0) ? KhensuTheme::bgSurface : KhensuTheme::bgSurface.brighter(0.04f));
        g.fillRect(area);
        switch (row.kind) {
        case RowKind::Spectrum: paintSpectrogramRow(g, area); break;
        case RowKind::Density: paintDensityRow(g, area); break;
        case RowKind::Data: paintDataRow(g, area, dataLanes_[static_cast<size_t>(row.index)]); break;
        case RowKind::Marker: paintMarkerRow(g, area, lanes_[static_cast<size_t>(row.index)]); break;
        }
        g.setColour(style().grid);
        g.drawHorizontalLine(juce::roundToInt(area.getBottom()), area.getX(), area.getRight());
    }
}

void MultiLaneHeatmap::paintSpectrogramRow(juce::Graphics& g, juce::Rectangle<float> area)
{
    const auto dst = juce::Rectangle<float>(px(0.0), area.getY(), px(total_) - px(0.0), area.getHeight()).toNearestInt();
    if (dst.isEmpty()) return;
    if (spectrogramImageDirty_ || spectrogramImage_.getWidth() != dst.getWidth() || spectrogramImage_.getHeight() != dst.getHeight()) {
        spectrogramImageDirty_ = false;
        spectrogramImage_ = buildSpectrogramImage(spectrogramFrames_, dst.getWidth(), dst.getHeight()); // VIS-2: magma
    }
    if (spectrogramImage_.isValid()) g.drawImageAt(spectrogramImage_, dst.getX(), dst.getY());
}

void MultiLaneHeatmap::paintDensityRow(juce::Graphics& g, juce::Rectangle<float> area)
{
    if (densityMax_ <= 0) return;
    const double bucketSec = densityBucketSeconds();
    const auto cell = area.reduced(0.0f, 2.0f);
    for (int b = 0; b < kDensityBuckets; ++b) {
        const int count = densityCounts_[static_cast<size_t>(b)];
        if (count <= 0) continue;
        const float x0 = px(bucketSec * b), x1 = px(bucketSec * (b + 1));
        g.setColour(densityColour(static_cast<float>(count) / static_cast<float>(densityMax_)));
        g.fillRect(juce::Rectangle<float>(x0, cell.getY(), std::max(1.0f, x1 - x0 - 1.0f), cell.getHeight()));
    }
}

void MultiLaneHeatmap::paintDataRow(juce::Graphics& g, juce::Rectangle<float> area, const TimelineDataLane& dl)
{
    const auto band = area.reduced(0.0f, 4.0f);
    const float range = std::max(1e-6f, dl.maxV - dl.minV);
    auto yFor = [&](float v) { return band.getBottom() - juce::jlimit(0.0f, 1.0f, (v - dl.minV) / range) * band.getHeight(); };

    // Zero / reference guide when the range straddles it (e.g. correlation).
    if (dl.minV < 0.0f && dl.maxV > 0.0f) {
        g.setColour(style().axis);
        g.drawHorizontalLine(juce::roundToInt(yFor(0.0f)), band.getX(), band.getRight());
    }

    if (dl.samples.size() >= 2) {
        // NaN samples (no data - e.g. a silent loudness window) break the line
        // into segments; each segment gets its own fill.
        juce::Path line, fill;
        bool penDown = false;
        float segStartX = 0.0f, lastX = 0.0f;
        auto closeSegment = [&] {
            if (!penDown) return;
            fill.lineTo(lastX, band.getBottom());
            fill.lineTo(segStartX, band.getBottom());
            fill.closeSubPath();
            penDown = false;
        };
        for (const auto& s : dl.samples) {
            if (!std::isfinite(s.y)) {
                closeSegment();
                continue;
            }
            const float X = px(s.x), Y = yFor(s.y);
            if (!penDown) {
                line.startNewSubPath(X, Y);
                fill.startNewSubPath(X, Y);
                segStartX = X;
                penDown = true;
            } else {
                line.lineTo(X, Y);
                fill.lineTo(X, Y);
            }
            lastX = X;
        }
        closeSegment();
        g.setColour(dl.colour.withAlpha(0.12f));
        g.fillPath(fill);
        g.setColour(dl.colour.withAlpha(0.9f));
        g.strokePath(line, juce::PathStrokeType(1.4f, juce::PathStrokeType::curved));
    }

    // The lane's value range, small, inside its top-left / bottom-left.
    g.setColour(style().muted);
    g.setFont(KhensuTheme::uiFont(type::caption, juce::Font::plain));
    const auto labels = area.reduced(4.0f, 1.0f);
    g.drawText(juce::String(juce::roundToInt(dl.maxV)) + dl.suffix, labels, juce::Justification::topLeft, false);
    g.drawText(juce::String(juce::roundToInt(dl.minV)) + dl.suffix, labels, juce::Justification::bottomLeft, false);
}

void MultiLaneHeatmap::paintMarkerRow(juce::Graphics& g, juce::Rectangle<float> area, const HeatmapLaneData& lane)
{
    for (const auto& rec : lane.recs) {
        if (!rec.time_range.has_value()) continue;
        const auto col = severityColour(rec.severity);
        if (rec.time_range->end_sec > rec.time_range->start_sec) {
            const float rx0 = px(rec.time_range->start_sec), rx1 = px(rec.time_range->end_sec);
            g.setColour(col.withAlpha(0.14f));
            g.fillRect(rx0, area.getY(), rx1 - rx0, area.getHeight());
        }
        const float mx = px(rec.time_range->start_sec), my = area.getCentreY();
        juce::Path p;
        p.addTriangle(mx, my - 6.0f, mx - 5.0f, my + 4.0f, mx + 5.0f, my + 4.0f);
        g.setColour(col);
        g.fillPath(p);
        g.setColour(juce::Colours::black); // theme-exempt(glyph on a saturated accent marker - black reads in every theme)
        g.setFont(KhensuTheme::uiFont(type::caption, juce::Font::bold));
        g.drawText("!", juce::Rectangle<float>(mx - 4.0f, my - 4.0f, 8.0f, 8.0f), juce::Justification::centred, false);
    }
}

void MultiLaneHeatmap::paintOverlay(juce::Graphics& g, juce::Rectangle<float> plot)
{
    juce::ignoreUnused(plot);
    // VIS-2: a colour bar under the name of each heat row, so a colour reads
    // as a value - the spectrogram map (quiet .. loud) and the sequential map
    // (few .. most findings). Only when the row has room under its label.
    for (int i = 0; i < rowCount(); ++i) {
        const auto& row = rows_[static_cast<size_t>(i)];
        if (row.kind != RowKind::Spectrum && row.kind != RowKind::Density) continue;
        const auto area = rowArea(i);
        if (area.getHeight() < 32.0f) continue;
        const auto bar = juce::Rectangle<float>(6.0f, area.getCentreY() + 9.0f, plotArea().getX() - 14.0f, 5.0f);
        const bool spectrum = row.kind == RowKind::Spectrum;
        auto at = [spectrum](float t) { return spectrum ? ui::viz::spectrogram(t) : densityColour(t); };
        juce::ColourGradient grad(at(0.0f), bar.getX(), 0.0f, at(1.0f), bar.getRight(), 0.0f, false);
        for (int k = 1; k < 8; ++k)
            grad.addColour(k / 8.0, at(static_cast<float>(k) / 8.0f));
        g.setGradientFill(grad);
        g.fillRoundedRectangle(bar, 2.0f);
    }
}

void MultiLaneHeatmap::paintHoverMark(juce::Graphics& g, juce::Rectangle<float> plot, float X, float Y)
{
    g.setColour(style().crosshair);
    g.drawVerticalLine(juce::roundToInt(X), plot.getY(), plot.getBottom());
    // The hovered row, outlined.
    const int row = rowAt(yAt(Y));
    if (row >= 0) {
        g.setColour(style().crosshair.withMultipliedAlpha(0.5f));
        g.drawRect(rowArea(row), 1.0f);
    }
}

ui::chart::Readout MultiLaneHeatmap::readoutAt(double xValue, double yValue)
{
    ui::chart::Readout r;
    const int rowIdx = rowAt(yValue);
    if (rowIdx < 0) return r;
    const auto& row = rows_[static_cast<size_t>(rowIdx)];
    const double sec = juce::jlimit(0.0, static_cast<double>(std::max(0.0f, total_)), xValue);
    r.title = row.name + "  " + x.label(sec, true);

    switch (row.kind) {
    case RowKind::Spectrum: {
        const auto& frames = spectrogramFrames_;
        if (frames.empty() || total_ <= 0.0f) break;
        const auto f = static_cast<size_t>(juce::jlimit(0, static_cast<int>(frames.size()) - 1,
                                                        static_cast<int>(sec / total_ * static_cast<double>(frames.size()))));
        const auto& frame = frames[f];
        if (frame.empty()) break;
        // Where in the row: bottom = lowest band. The service's bands are
        // log-spaced over the standard frequency axis (VIS-3), so the
        // height in the row reads as a frequency on that axis.
        const auto area = rowArea(rowIdx);
        const float yPix = py(yValue);
        const float up = area.getHeight() > 0.0f ? juce::jlimit(0.0f, 0.999f, (area.getBottom() - yPix) / area.getHeight()) : 0.0f;
        const auto band = static_cast<size_t>(up * static_cast<float>(frame.size()));
        const float v = std::isfinite(frame[band]) ? juce::jlimit(0.0f, 1.0f, frame[band]) : 0.0f;
        r.lines.push_back({TRANS("Around") + " " + ui::viz::freqReadout(ui::viz::freqAt(up)), style().muted});
        r.lines.push_back({TRANS("Level") + " " + juce::String(juce::roundToInt(v * 100.0f)) + " % " + TRANS("of the loudest"),
                           ui::viz::spectrogram(v)});
        break;
    }
    case RowKind::Density: {
        const double bucketSec = densityBucketSeconds();
        if (bucketSec <= 0.0) break;
        const int b = juce::jlimit(0, kDensityBuckets - 1, static_cast<int>(sec / bucketSec));
        const int count = densityCounts_[static_cast<size_t>(b)];
        const auto span = x.label(bucketSec * b) + " - " + x.label(bucketSec * (b + 1));
        r.lines.push_back({juce::String(count) + " " + (count == 1 ? TRANS("finding") : TRANS("findings")) + " (" + span + ")",
                           count > 0 && densityMax_ > 0 ? densityColour(static_cast<float>(count) / static_cast<float>(densityMax_))
                                                         : style().muted});
        break;
    }
    case RowKind::Data: {
        const auto& dl = dataLanes_[static_cast<size_t>(row.index)];
        const float v = nearestValue(dl, sec);
        r.lines.push_back({std::isfinite(v) ? formatValue(v, dl.suffix) : TRANS("No data here"), dl.colour});
        break;
    }
    case RowKind::Marker: {
        // Findings under the pointer: inside their range, or near their start.
        const double near = std::max(1.5, 0.02 * static_cast<double>(total_));
        for (const auto& rec : lanes_[static_cast<size_t>(row.index)].recs) {
            if (!rec.time_range.has_value()) continue;
            const double s0 = rec.time_range->start_sec, s1 = rec.time_range->end_sec;
            if ((s1 > s0 && sec >= s0 && sec <= s1) || std::abs(sec - s0) <= near)
                r.lines.push_back({juce::String(rec.what), severityColour(rec.severity)});
        }
        if (r.lines.empty()) r.lines.push_back({TRANS("No finding here"), style().muted});
        break;
    }
    }
    return r;
}

juce::String MultiLaneHeatmap::autoSummary() const
{
    if (isEmpty()) return TRANS("Issue heatmap: no timeline data yet.");
    juce::String s = TRANS("Issue heatmap over") + " " + x.label(total_) + ": ";
    juce::StringArray counts;
    int total = 0;
    for (const auto& lane : lanes_) {
        counts.add(lane.label + " " + juce::String(static_cast<int>(lane.recs.size())));
        total += static_cast<int>(lane.recs.size());
    }
    s << (counts.isEmpty() ? TRANS("no finding lanes") : TRANS("findings by lane") + " - " + ui::chart::joinWords(counts));
    for (const auto& dl : dataLanes_) {
        const auto [lo, hi] = dataRange(dl);
        if (lo > hi) continue;
        s << "; " << dl.label << " " << formatValue(lo, dl.suffix) << " " << TRANS("to") << " " << formatValue(hi, dl.suffix);
    }
    if (densityMax_ > 0 && total > 1) {
        const auto it = std::max_element(densityCounts_.begin(), densityCounts_.end());
        const auto b = static_cast<int>(it - densityCounts_.begin());
        s << "; " << TRANS("most findings around") << " " << x.label(densityBucketSeconds() * (b + 0.5));
    }
    if (!spectrogramFrames_.empty()) s << "; " << TRANS("with a spectrogram row");
    return s + ".";
}

} // namespace khensu
