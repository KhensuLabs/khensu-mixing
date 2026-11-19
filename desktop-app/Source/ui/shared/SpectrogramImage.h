// desktop-app/Source/ui/shared/SpectrogramImage.h
#pragma once
#include <JuceHeader.h>
#include <vector>

namespace khensu {

/// Rasterizes a [frame][band] magnitude grid (0..1 normalized, as returned
/// by analyzeSpectrotemporal) into a heat-coloured image - shared by
/// MultiLaneHeatmap's compact Timeline-tab strip and SpectrogramPanel's full
/// dedicated view, so the two always render pixel-identical output from one
/// code path instead of two copies that could visually drift apart.
juce::Image buildSpectrogramImage(const std::vector<std::vector<float>>& frames, int width, int height);

} // namespace khensu
