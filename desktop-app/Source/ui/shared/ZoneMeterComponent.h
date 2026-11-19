// desktop-app/Source/ui/shared/ZoneMeterComponent.h
#pragma once
#include <JuceHeader.h>
#include <vector>
#include "KhensuTheme.h"

namespace khensu {

struct ZoneLabel {
    float normPos = 0.0f;
    juce::String text;
};

class ZoneMeterComponent final : public juce::Component {
  public:
    ZoneMeterComponent() = default;

    // API compatible with MeterBarComponent
    void setRange(float minVal, float maxVal);
    void setTargetZone(float lo, float hi);
    void setValue(float v, const juce::String& caption = {});

    // New API
    void addZoneLabel(float normPos, const juce::String& label);
    void clearZoneLabels();

    // Testable pure logic (public for testing)
    float normalise(float v) const;
    juce::Colour fillColourForValue(float v) const;

    void paint(juce::Graphics& g) override;

  private:
    float min_ = 0.0f, max_ = 1.0f;
    float zoneLo_ = 0.0f, zoneHi_ = 1.0f;
    float value_ = 0.0f, displayValue_ = 0.0f;
    juce::String caption_;
    std::vector<ZoneLabel> zoneLabels_;

    /// CE-PLT-4: display-rate, time-based ease (VBlankAttachment).
    void onVBlank(double timestampSec);
    bool animating_ = false;
    double lastFrameSec_ = -1.0;
    juce::VBlankAttachment vblank_{this, [this](double t) { onVBlank(t); }};
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ZoneMeterComponent)
};

} // namespace khensu
