// desktop-app/Source/ui/shared/HeatmapImage.h
// Builds a juce::Image from a [frame][band] grid via a value->colour function,
// so time x frequency heatmaps (mid/side, phase, spectral-delta) render as one
// scaled image instead of thousands of fillRect calls per repaint. Frame index
// maps to X (time), band index to Y with band 0 at the BOTTOM (low freq).
//
// VIS-3: buildGridImageOnAxis() places the bands by their edges on the one
// frequency axis (ui::viz, 20 Hz - 20 kHz log), so a heat map lines up with
// every other spectrum; gridReadoutOnAxis() is its hover text.
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include <khensu/ui/VizFrequencyAxis.h>
#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

namespace khensu {

inline juce::Image buildGridImage(const std::vector<std::vector<float>>& grid,
                                  const std::function<juce::Colour(float)>& colourFn)
{
    const int w = (int)grid.size();
    const int h = w > 0 ? (int)grid[0].size() : 0;
    if (w == 0 || h == 0) return {};

    juce::Image img(juce::Image::ARGB, w, h, true);
    juce::Image::BitmapData bmp(img, juce::Image::BitmapData::writeOnly);
    for (int x = 0; x < w; ++x) {
        const auto& col = grid[(size_t)x];
        for (int y = 0; y < h && y < (int)col.size(); ++y) {
            // Band 0 (low freq) at the bottom row.
            bmp.setPixelColour(x, h - 1 - y, colourFn(col[(size_t)y]));
        }
    }
    return img;
}

/// The band holding `hz` given its edges (bands + 1 ascending values), or -1
/// outside them. Without usable edges the bands are taken as evenly spread
/// (log) across the standard axis.
inline int bandAtHz(const std::vector<float>& edgesHz, int bands, float hz)
{
    if (bands <= 0) return -1;
    if (static_cast<int>(edgesHz.size()) != bands + 1) {
        const int b = static_cast<int>(ui::viz::freqNorm(hz) * static_cast<float>(bands));
        return juce::jlimit(0, bands - 1, b);
    }
    if (hz < edgesHz.front() || hz > edgesHz.back()) return -1;
    const auto it = std::upper_bound(edgesHz.begin(), edgesHz.end(), hz);
    return juce::jlimit(0, bands - 1, static_cast<int>(it - edgesHz.begin()) - 1);
}

/// The grid drawn on the standard frequency axis: one column per frame,
/// `rows` rows from 20 kHz (top) to 20 Hz (bottom); a row outside the band
/// edges stays transparent (not measured).
inline juce::Image buildGridImageOnAxis(const std::vector<std::vector<float>>& grid, const std::vector<float>& edgesHz,
                                        const std::function<juce::Colour(float)>& colourFn, int rows = 192)
{
    const int w = static_cast<int>(grid.size());
    const int bands = w > 0 ? static_cast<int>(grid[0].size()) : 0;
    if (w == 0 || bands == 0 || rows <= 0) return {};
    std::vector<int> bandOfRow(static_cast<size_t>(rows));
    for (int y = 0; y < rows; ++y) {
        const float t = 1.0f - (static_cast<float>(y) + 0.5f) / static_cast<float>(rows);
        bandOfRow[static_cast<size_t>(y)] = bandAtHz(edgesHz, bands, ui::viz::freqAt(t));
    }
    juce::Image img(juce::Image::ARGB, w, rows, true);
    juce::Image::BitmapData bmp(img, juce::Image::BitmapData::writeOnly);
    for (int x = 0; x < w; ++x) {
        const auto& col = grid[static_cast<size_t>(x)];
        for (int y = 0; y < rows; ++y) {
            const int b = bandOfRow[static_cast<size_t>(y)];
            if (b < 0 || b >= static_cast<int>(col.size())) continue;
            bmp.setPixelColour(x, y, colourFn(col[static_cast<size_t>(b)]));
        }
    }
    return img;
}

/// VIZ-U3 / VIS-3: the value under `pos` in a grid drawn with
/// buildGridImageOnAxis over `area`, with its time and frequency.
inline juce::String gridReadoutOnAxis(const std::vector<std::vector<float>>& grid, const std::vector<float>& edgesHz,
                                      float durationSec, juce::Rectangle<float> area, juce::Point<float> pos,
                                      const std::function<juce::String(float)>& value)
{
    if (grid.empty() || grid[0].empty() || !area.contains(pos) || area.getWidth() <= 0.0f) return {};
    const auto f = static_cast<size_t>(juce::jlimit(
        0, static_cast<int>(grid.size()) - 1,
        static_cast<int>((pos.x - area.getX()) / area.getWidth() * static_cast<float>(grid.size()))));
    const int bands = static_cast<int>(grid[f].size());
    const float hz = ui::viz::freqAt((area.getBottom() - pos.y) / area.getHeight());
    juce::String where;
    if (durationSec > 0.0f) {
        const int t = juce::roundToInt(durationSec * (static_cast<float>(f) + 0.5f) / static_cast<float>(grid.size()));
        where << t / 60 << ":" << juce::String(t % 60).paddedLeft('0', 2) << ", ";
    }
    where << ui::viz::freqReadout(hz);
    const int b = bandAtHz(edgesHz, bands, hz);
    if (b < 0) return where + ": " + TRANS("not measured");
    const float v = grid[f][static_cast<size_t>(b)];
    return where + ": " + (std::isfinite(v) ? value(v) : TRANS("no data"));
}

} // namespace khensu
