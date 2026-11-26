// desktop-app/Source/ui/shared/RadarChartComponent.h
// VIS-1: the radar charts (Heaviness, Sonic Fingerprint) are the ChartKit
// RadarChart (khensu/ui/ChartKitPlots.h) in the desktop theme: labelled
// rings, each spoke's name and value, the hover readout and a screen-reader
// summary. The track is the first categorical colour, the genre's profile
// a dashed median-grey outline.
#pragma once
#include <JuceHeader.h>
#include <khensu/ui/ChartKitPlots.h>
#include <vector>
#include "KhensuTheme.h"

namespace khensu {

struct RadarAxis {
    juce::String label;
    float value = 0.0f;     ///< 0..1 along the spoke
    juce::String valueText; ///< the measured value with its unit ("-14.2 LUFS"); default a percent
};

// THREAD_CONTEXT(UIThread)
class RadarChartComponent final : public ui::chart::RadarChart {
  public:
    RadarChartComponent();
    void setAxes(std::vector<RadarAxis> axes);
    /// The genre's profile, one value per axis; `name` names it in the key.
    void setReference(std::vector<float> values, const juce::String& name = {});

  private:
    void applyThemeColours();
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RadarChartComponent)
};

} // namespace khensu
