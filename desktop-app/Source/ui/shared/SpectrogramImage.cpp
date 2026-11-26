// desktop-app/Source/ui/shared/SpectrogramImage.cpp
#include "SpectrogramImage.h"
#include "VizMaps.h"
#include <cmath>

namespace khensu {

juce::Image buildSpectrogramImage(const std::vector<std::vector<float>>& frames, int width, int height)
{
    if (width <= 0 || height <= 0 || frames.empty() || frames[0].empty()) return {};

    juce::Image image(juce::Image::RGB, width, height, false);
    juce::Image::BitmapData bmp(image, juce::Image::BitmapData::writeOnly);

    const auto& lut = ui::viz::spectrogramLut(); // VIS-2: the one spectrogram map (magma)
    const int nFrames = (int)frames.size();
    const int nBands = (int)frames[0].size();

    for (int x = 0; x < width; ++x) {
        const int frameIdx = juce::jlimit(0, nFrames - 1, x * nFrames / juce::jmax(1, width));
        const auto& frame = frames[(size_t)frameIdx];
        for (int y = 0; y < height; ++y) {
            // Row 0 = top = highest frequency band, matching the visual
            // convention of a spectrogram (bass at the bottom).
            const int bandIdx = juce::jlimit(0, nBands - 1, nBands - 1 - (y * nBands / juce::jmax(1, height)));
            const float mag = bandIdx < (int)frame.size() ? frame[(size_t)bandIdx] : 0.0f;
            const int idx = std::isfinite(mag) ? juce::jlimit(0, 255, juce::roundToInt(mag * 255.0f)) : 0;
            bmp.setPixelColour(x, y, juce::Colour(lut[(size_t)idx].getInARGBMaskOrder()));
        }
    }
    return image;
}

} // namespace khensu
