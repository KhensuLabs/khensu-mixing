// desktop-app/Source/ui/shared/SpectrogramImage.cpp
#include "SpectrogramImage.h"

namespace khensu {

juce::Image buildSpectrogramImage(const std::vector<std::vector<float>>& frames, int width, int height)
{
    if (width <= 0 || height <= 0 || frames.empty() || frames[0].empty()) return {};

    juce::Image image(juce::Image::RGB, width, height, false);
    juce::Image::BitmapData bmp(image, juce::Image::BitmapData::writeOnly);

    const int nFrames = (int)frames.size();
    const int nBands = (int)frames[0].size();

    for (int x = 0; x < width; ++x) {
        const int frameIdx = juce::jlimit(0, nFrames - 1, x * nFrames / juce::jmax(1, width));
        const auto& frame = frames[(size_t)frameIdx];
        for (int y = 0; y < height; ++y) {
            // Row 0 = top = highest frequency band, matching the visual
            // convention of a spectrogram (bass at the bottom).
            const int bandIdx = juce::jlimit(0, nBands - 1, nBands - 1 - (y * nBands / juce::jmax(1, height)));
            const float mag = frame[(size_t)bandIdx];
            // Dark teal -> bright amber, matching the app's existing heat colour language.
            const juce::Colour col = juce::Colour(0xff0a1a1a).interpolatedWith(juce::Colour(0xffffb020), mag); // theme-exempt(data-viz ramp/palette: perceptual scale, deliberately theme-independent)
            bmp.setPixelColour(x, y, col);
        }
    }
    return image;
}

} // namespace khensu
