// desktop-app/Source/ui/shared/MeterBarComponent.h
#pragma once
#include <JuceHeader.h>

namespace khensu {

// Horizontal meter: a track with a fill proportional to value within [min,max],
// an optional target band, and a caption.
// THREAD_CONTEXT(UIThread)
class MeterBarComponent final : public juce::Component {
  public:
    MeterBarComponent() = default;
    void setRange(float minVal, float maxVal);
    void setTargetZone(float lo, float hi);
    void setValue(float v, const juce::String& label);
    void paint(juce::Graphics& g) override;

  private:
    float min_ = 0.0f, max_ = 1.0f, value_ = 0.0f;
    float zoneLo_ = 0.0f, zoneHi_ = 0.0f;
    float peakHold_ = 0.0f;
    float displayValue_ = 0.0f; // eased toward value_ each display frame
    juce::String label_;
    float norm(float v) const;
    /// CE-PLT-4: animation runs on the display's refresh (VBlankAttachment),
    /// time-based so it looks the same at 60 and 120 Hz. Idle frames return
    /// at once.
    void onVBlank(double timestampSec);
    bool animating_ = false;
    double lastFrameSec_ = -1.0;
    juce::VBlankAttachment vblank_{this, [this](double t) { onVBlank(t); }};
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MeterBarComponent)
};

} // namespace khensu
