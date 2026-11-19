// desktop-app/Source/ui/Dashboard/MultiLaneHeatmap.cpp
#include "MultiLaneHeatmap.h"
#include "../shared/KhensuLookAndFeel.h"
#include "../shared/SpectrogramImage.h"
#include "../../IAppController.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include "../shared/KhensuTheme.h"

namespace khensu {

MultiLaneHeatmap::MultiLaneHeatmap(IAppController* controller) : controller_(controller)
{
    setMouseCursor(juce::MouseCursor::CrosshairCursor);
    if (controller_ != nullptr) vblank_ = std::make_unique<juce::VBlankAttachment>(this, [this] { onVBlank(); });
}

MultiLaneHeatmap::~MultiLaneHeatmap() { vblank_.reset(); }

void MultiLaneHeatmap::setSpan(float t)
{
    total_ = t;
    rebuildDensity();
    repaint();
}

void MultiLaneHeatmap::setLanes(std::vector<HeatmapLaneData> lanes)
{
    lanes_ = std::move(lanes);
    rebuildDensity();
    repaint();
}

void MultiLaneHeatmap::setDensityEnabled(bool enabled)
{
    densityEnabled_ = enabled;
    rebuildDensity();
    repaint();
}

void MultiLaneHeatmap::setDataLanes(std::vector<TimelineDataLane> dataLanes)
{
    dataLanes_ = std::move(dataLanes);
    repaint();
}

void MultiLaneHeatmap::rebuildDensity()
{
    densityCounts_.assign(kDensityBuckets, 0.0f);
    if (total_ <= 0.0f) return;
    for (const auto& lane : lanes_) {
        for (const auto& rec : lane.recs) {
            if (!rec.time_range.has_value()) continue;
            const float t = rec.time_range->start_sec;
            int bucket = (int)juce::jlimit(0.0f, (float)kDensityBuckets - 1.0f, (t / total_) * (float)kDensityBuckets);
            densityCounts_[(size_t)bucket] += 1.0f;
        }
    }
    const float maxCount = *std::max_element(densityCounts_.begin(), densityCounts_.end());
    if (maxCount > 0.0f)
        for (auto& c : densityCounts_)
            c /= maxCount;
}

void MultiLaneHeatmap::setSpectrogram(std::vector<std::vector<float>> frames)
{
    spectrogramFrames_ = std::move(frames);
    spectrogramImageDirty_ = true;
    repaint();
}

void MultiLaneHeatmap::onVBlank()
{
    if (controller_ == nullptr) return;
    const float pos = static_cast<float>(controller_->playback().getPlaybackPosition());
    if (std::abs(playheadSec_ - pos) <= 0.001f) return;
    // paint(): a 2 px full-height line plus a 10 px head; pad for AA.
    const auto strip = [this](float sec) {
        return juce::Rectangle<int>(juce::roundToInt(xFor(sec)) - 7, 0, 14, getHeight());
    };
    const float old = playheadSec_;
    playheadSec_ = pos;
    if (old >= 0.0f && pos >= 0.0f && juce::roundToInt(xFor(old)) == juce::roundToInt(xFor(pos))) return;
    if (old >= 0.0f) repaint(strip(old));
    if (pos >= 0.0f) repaint(strip(pos));
}

float MultiLaneHeatmap::xFor(float sec) const
{
    if (total_ <= 0.0f) return 0.0f;
    const float w = (float)getWidth() - (float)kLabelW;
    return (float)kLabelW + juce::jlimit(0.0f, 1.0f, sec / total_) * w;
}

float MultiLaneHeatmap::timeForX(float x) const
{
    const float w = (float)getWidth() - (float)kLabelW;
    if (w <= 0.0f) return 0.0f;
    return juce::jlimit(0.0f, total_, ((x - (float)kLabelW) / w) * total_);
}

juce::Rectangle<float> MultiLaneHeatmap::axisArea() const { return {0.0f, 0.0f, (float)getWidth(), (float)kAxisH}; }

juce::Rectangle<float> MultiLaneHeatmap::spectrogramArea() const
{
    if (spectrogramFrames_.empty()) return {};
    return {(float)kLabelW, (float)kAxisH, (float)(getWidth() - kLabelW), (float)kSpectrogramH};
}

juce::Rectangle<float> MultiLaneHeatmap::densityArea() const
{
    if (!densityEnabled_) return {};
    const float top = (float)kAxisH + (spectrogramFrames_.empty() ? 0.0f : (float)kSpectrogramH);
    return {(float)kLabelW, top, (float)(getWidth() - kLabelW), (float)kDensityH};
}

float MultiLaneHeatmap::dataLanesTop() const
{
    return (float)kAxisH + (spectrogramFrames_.empty() ? 0.0f : (float)kSpectrogramH) +
           (densityEnabled_ ? (float)kDensityH : 0.0f);
}

float MultiLaneHeatmap::markerLanesTop() const { return dataLanesTop() + (float)dataLanes_.size() * (float)kDataLaneH; }

juce::Rectangle<float> MultiLaneHeatmap::dataLaneArea(int index) const
{
    if (index < 0 || index >= (int)dataLanes_.size()) return {};
    return {0.0f, dataLanesTop() + (float)kDataLaneH * (float)index, (float)getWidth(), (float)kDataLaneH};
}

juce::Rectangle<float> MultiLaneHeatmap::laneArea(int index) const
{
    const int n = laneCount();
    if (n <= 0) return {};
    const float top = markerLanesTop();
    const float laneH = ((float)getHeight() - top) / (float)n;
    return {0.0f, top + laneH * (float)index, (float)getWidth(), laneH};
}

void MultiLaneHeatmap::rebuildSpectrogramImage(int width, int height)
{
    spectrogramImageDirty_ = false;
    spectrogramImage_ = buildSpectrogramImage(spectrogramFrames_, width, height);
}

void MultiLaneHeatmap::paint(juce::Graphics& g)
{
    if (total_ <= 0.0f || (lanes_.empty() && dataLanes_.empty())) {
        g.setColour(KhensuTheme::textSecondary.withAlpha(0.5f));
        g.setFont(khensu::KhensuTheme::uiFont(10.0f, juce::Font::italic));
        g.drawText("No timeline data", getLocalBounds(), juce::Justification::centred, false);
        return;
    }

    // --- Shared time axis ---
    auto axis = axisArea();
    g.setColour(KhensuTheme::bgSurface.darker(0.1f));
    g.fillRect(axis);

    // Pick a tick interval that keeps ~6-10 ticks across the span.
    static constexpr float kSteps[] = {5.0f, 10.0f, 15.0f, 30.0f, 60.0f, 120.0f, 300.0f, 600.0f};
    float step = kSteps[std::size(kSteps) - 1];
    for (float s : kSteps) {
        if (total_ / s <= 10.0f) {
            step = s;
            break;
        }
    }

    g.setColour(KhensuTheme::textSecondary.withAlpha(0.6f));
    g.setFont(khensu::KhensuTheme::uiFont(9.0f, juce::Font::plain));
    for (float t = 0.0f; t <= total_; t += step) {
        const float x = xFor(t);
        g.drawVerticalLine(juce::roundToInt(x), axis.getBottom() - 5.0f, axis.getBottom());
        const int min = (int)t / 60, sec = (int)t % 60;
        g.drawText(juce::String(min) + ":" + juce::String(sec).paddedLeft('0', 2), (int)x - 20, 0, 40, kAxisH - 5,
                   juce::Justification::centred, false);
    }

    // --- Spectro-temporal heatmap row (shares the axis above) ---
    if (!spectrogramFrames_.empty()) {
        auto specArea = spectrogramArea();
        if (spectrogramImageDirty_ || spectrogramImage_.getWidth() != (int)specArea.getWidth() ||
            spectrogramImage_.getHeight() != (int)specArea.getHeight())
            rebuildSpectrogramImage((int)specArea.getWidth(), (int)specArea.getHeight());
        g.drawImageAt(spectrogramImage_, (int)specArea.getX(), (int)specArea.getY());

        g.setColour(KhensuTheme::textSecondary.withAlpha(0.5f));
        g.setFont(khensu::KhensuTheme::uiFont(8.0f, juce::Font::plain));
        g.drawText("SPECTRUM", 2, (int)specArea.getY(), kLabelW - 4, 10, juce::Justification::centredLeft, false);
    }

    // --- Issue density histogram row (shares the axis above) ---
    if (densityEnabled_ && !densityCounts_.empty()) {
        auto densArea = densityArea();
        g.setColour(KhensuTheme::bgSurface.darker(0.05f));
        g.fillRect(densArea);
        g.setColour(KhensuTheme::textSecondary.withAlpha(0.5f));
        g.setFont(khensu::KhensuTheme::uiFont(8.0f, juce::Font::plain));
        g.drawText("DENSITY", 2, (int)densArea.getY(), kLabelW - 4, 10, juce::Justification::centredLeft, false);

        const float bw = densArea.getWidth() / (float)kDensityBuckets;
        for (int i = 0; i < kDensityBuckets; ++i) {
            const float frac = densityCounts_[(size_t)i];
            if (frac <= 0.0f) continue;
            const float barH = frac * densArea.getHeight();
            g.setColour(KhensuTheme::accentRed.withAlpha(0.25f + 0.6f * frac));
            g.fillRect(densArea.getX() + bw * (float)i, densArea.getBottom() - barH, bw, barH);
        }
    }

    // --- Continuous data lanes (loudness curve, stereo correlation, ...) ---
    for (int i = 0; i < (int)dataLanes_.size(); ++i) {
        const auto& dl = dataLanes_[(size_t)i];
        auto area = dataLaneArea(i);
        auto plot = area.withTrimmedLeft((float)kLabelW).reduced(0.0f, 4.0f);

        g.setColour((i % 2 == 0) ? KhensuTheme::bgSurface.brighter(0.02f) : KhensuTheme::bgSurface);
        g.fillRect(area.withTrimmedLeft((float)kLabelW));

        // Lane label + value range.
        g.setColour(KhensuTheme::textSecondary);
        g.setFont(khensu::KhensuTheme::uiFont(9.0f, juce::Font::bold));
        g.drawText(dl.label, (int)area.getX() + 2, (int)area.getY() + 2, kLabelW - 4, 10,
                   juce::Justification::centredLeft, false);
        g.setColour(KhensuTheme::textSecondary.withAlpha(0.5f));
        g.setFont(khensu::KhensuTheme::uiFont(7.5f, juce::Font::plain));
        g.drawText(juce::String(juce::roundToInt(dl.maxV)) + dl.suffix, (int)area.getX() + 2, (int)plot.getY() - 1, kLabelW - 4, 9,
                   juce::Justification::centredLeft, false);
        g.drawText(juce::String(juce::roundToInt(dl.minV)) + dl.suffix, (int)area.getX() + 2, (int)plot.getBottom() - 8, kLabelW - 4,
                   9, juce::Justification::centredLeft, false);

        const float range = juce::jmax(1e-6f, dl.maxV - dl.minV);
        auto yFor = [&](float v) {
            return plot.getBottom() - juce::jlimit(0.0f, 1.0f, (v - dl.minV) / range) * plot.getHeight();
        };

        // Zero/reference guide when the range straddles it (e.g. correlation).
        if (dl.minV < 0.0f && dl.maxV > 0.0f) {
            g.setColour(KhensuTheme::textSecondary.withAlpha(0.18f));
            g.drawHorizontalLine((int)yFor(0.0f), plot.getX(), plot.getRight());
        }

        // The line + a soft fill under it.
        if (dl.samples.size() >= 2) {
            // NaN samples (no data - e.g. a silent loudness window) break the
            // line into separate segments; each segment gets its own fill.
            juce::Path line, fill;
            bool penDown = false;
            float segStartX = 0.0f, lastX = 0.0f;
            auto closeSegment = [&] {
                if (!penDown) return;
                fill.lineTo(lastX, plot.getBottom());
                fill.lineTo(segStartX, plot.getBottom());
                fill.closeSubPath();
                penDown = false;
            };
            for (const auto& s : dl.samples) {
                if (!std::isfinite(s.y)) {
                    closeSegment();
                    continue;
                }
                const float x = xFor(s.x);
                const float y = yFor(s.y);
                if (!penDown) {
                    line.startNewSubPath(x, y);
                    fill.startNewSubPath(x, y);
                    segStartX = x;
                    penDown = true;
                } else {
                    line.lineTo(x, y);
                    fill.lineTo(x, y);
                }
                lastX = x;
            }
            closeSegment();
            g.setColour(dl.colour.withAlpha(0.10f));
            g.fillPath(fill);
            g.setColour(dl.colour.withAlpha(0.85f));
            g.strokePath(line, juce::PathStrokeType(1.4f, juce::PathStrokeType::curved));
        }

        // Value chip at the hovered time.
        if (hoverTimeSec_ >= 0.0f && dl.samples.size() >= 2) {
            float best = std::numeric_limits<float>::quiet_NaN(), bestDt = 1e9f;
            for (const auto& s : dl.samples) {
                if (!std::isfinite(s.y)) continue; // no data there
                const float dt = std::abs(s.x - hoverTimeSec_);
                if (dt < bestDt) {
                    bestDt = dt;
                    best = s.y;
                }
            }
            if (std::isfinite(best)) { // the lane may have no data at all
            const float hx = xFor(hoverTimeSec_);
            const float hy = yFor(best);
            g.setColour(dl.colour);
            g.fillEllipse(hx - 2.5f, hy - 2.5f, 5.0f, 5.0f);
            juce::String chip = juce::String(best, 1) + dl.suffix;
            g.setFont(khensu::KhensuTheme::uiFont(8.5f, juce::Font::plain));
            const int cw = juce::GlyphArrangement::getStringWidthInt(g.getCurrentFont(), chip) + 8;
            int cx = juce::jlimit((int)plot.getX(), (int)plot.getRight() - cw, (int)hx + 6);
            g.setColour(KhensuTheme::bgVoid.withAlpha(0.9f));
            g.fillRoundedRectangle((float)cx, hy - 14.0f, (float)cw, 12.0f, 2.0f);
            g.setColour(dl.colour.brighter(0.2f));
            g.drawText(chip, cx, (int)hy - 14, cw, 12, juce::Justification::centred, false);
            }
        }

        g.setColour(KhensuTheme::textSecondary.withAlpha(0.2f));
        g.drawHorizontalLine(juce::roundToInt(area.getBottom()), area.getX(), area.getRight());
    }

    // --- Lanes ---
    const int n = laneCount();
    const Recommendation* hoveredRec = nullptr;
    float hoveredRecTime = -1.0f;

    for (int i = 0; i < n; ++i) {
        auto lane = laneArea(i);

        g.setColour((i % 2 == 0) ? KhensuTheme::bgSurface : KhensuTheme::bgSurface.brighter(0.04f));
        g.fillRect(lane.withTrimmedLeft((float)kLabelW));

        g.setColour(KhensuTheme::textSecondary);
        g.setFont(khensu::KhensuTheme::uiFont(9.0f, juce::Font::bold));
        g.drawText(lanes_[(size_t)i].label, lane.withWidth((float)kLabelW).toNearestInt(),
                   juce::Justification::centredLeft, false);

        for (const auto& rec : lanes_[(size_t)i].recs) {
            if (!rec.time_range.has_value()) continue;
            const float tStart = rec.time_range->start_sec;

            juce::Colour markerCol = rec.severity >= 66.0f   ? KhensuTheme::accentRed
                                     : rec.severity >= 33.0f ? KhensuTheme::accentAmber
                                                             : KhensuTheme::accentTeal;

            if (rec.time_range->end_sec > rec.time_range->start_sec) {
                const float rx0 = xFor(rec.time_range->start_sec);
                const float rx1 = xFor(rec.time_range->end_sec);
                g.setColour(markerCol.withAlpha(0.12f));
                g.fillRect(rx0, lane.getY(), rx1 - rx0, lane.getHeight());
            }

            const float mx = xFor(tStart);
            const float my = lane.getY() + lane.getHeight() * 0.5f;
            g.setColour(markerCol);
            juce::Path p;
            p.addTriangle(mx, my - 6.0f, mx - 5.0f, my + 4.0f, mx + 5.0f, my + 4.0f);
            g.fillPath(p);
            g.setColour(juce::Colours::black); // theme-exempt(glyph on a saturated accent marker - black reads in every theme)
            g.setFont(khensu::KhensuTheme::uiFont(9.0f, juce::Font::bold));
            g.drawText("!", (int)mx - 4, (int)my - 4, 8, 8, juce::Justification::centred);

            if (hoverTimeSec_ >= 0.0f && std::abs(hoverTimeSec_ - tStart) < 3.5f) {
                hoveredRec = &rec;
                hoveredRecTime = tStart;
            }
        }

        g.setColour(KhensuTheme::textSecondary.withAlpha(0.2f));
        g.drawHorizontalLine(juce::roundToInt(lane.getBottom()), lane.getX(), lane.getRight());
    }

    // --- Shared playhead spanning axis + all lanes ---
    if (playheadSec_ >= 0.0f) {
        const float px = xFor(playheadSec_);
        g.setColour(KhensuTheme::accentRed);
        g.fillRect(px - 1.0f, 0.0f, 2.0f, (float)getHeight());
        juce::Path head;
        head.addTriangle(px, 6.0f, px - 5.0f, 0.0f, px + 5.0f, 0.0f);
        g.fillPath(head);
    }

    // --- Hover readout ---
    if (hoverTimeSec_ >= 0.0f) {
        const float hx = xFor(hoverTimeSec_);
        g.setColour(khensu::KhensuTheme::textPrimary.withAlpha(0.3f));
        g.drawVerticalLine(juce::roundToInt(hx), (float)kAxisH, (float)getHeight());

        juce::String hoverText;
        juce::Colour tipCol = KhensuTheme::bgVoid;
        if (hoveredRec != nullptr) {
            hoverText = hoveredRec->what;
        } else {
            const int min = (int)hoverTimeSec_ / 60, sec = (int)hoverTimeSec_ % 60;
            hoverText = juce::String(min) + ":" + juce::String(sec).paddedLeft('0', 2);
        }

        g.setFont(khensu::KhensuTheme::uiFont(10.0f, juce::Font::plain));
        const int textW = juce::GlyphArrangement::getStringWidthInt(g.getCurrentFont(), hoverText) + 16;
        const int textH = 18;
        int tx = (int)hx - textW / 2;
        tx = juce::jlimit(kLabelW, getWidth() - textW - 4, tx);

        g.setColour(tipCol.withAlpha(0.92f));
        g.fillRoundedRectangle((float)tx, (float)kAxisH + 2.0f, (float)textW, (float)textH, 3.0f);
        g.setColour(khensu::KhensuTheme::textPrimary.withAlpha(0.9f));
        g.drawText(hoverText, tx, kAxisH + 2, textW, textH, juce::Justification::centred, false);
        juce::ignoreUnused(hoveredRecTime);
    }
}

void MultiLaneHeatmap::mouseMove(const juce::MouseEvent& e)
{
    if (total_ <= 0.0f) return;
    hoverTimeSec_ = timeForX((float)e.x);
    repaint();
}

void MultiLaneHeatmap::mouseExit(const juce::MouseEvent&)
{
    hoverTimeSec_ = -1.0f;
    repaint();
}

void MultiLaneHeatmap::mouseDoubleClick(const juce::MouseEvent& e)
{
    if (total_ <= 0.0f) return;
    if (seekCallback_) seekCallback_(timeForX((float)e.x));
}

} // namespace khensu
