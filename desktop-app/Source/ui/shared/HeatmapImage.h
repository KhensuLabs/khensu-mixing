// desktop-app/Source/ui/shared/HeatmapImage.h
// Builds a juce::Image from a [frame][band] grid via a value->colour function,
// so time x frequency heatmaps (mid/side, phase, spectral-delta) render as one
// scaled image instead of thousands of fillRect calls per repaint. Frame index
// maps to X (time), band index to Y with band 0 at the BOTTOM (low freq).
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
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

} // namespace khensu
