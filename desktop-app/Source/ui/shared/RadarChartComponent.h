// desktop-app/Source/ui/shared/RadarChartComponent.h
#pragma once
#include <JuceHeader.h>
#include <vector>

namespace khensu {

struct RadarAxis {
    juce::String label;
    float value = 0.0f;
}; // value 0..1

// THREAD_CONTEXT(UIThread)
class RadarChartComponent final : public juce::Component {
  public:
    RadarChartComponent() = default;
    void setAxes(std::vector<RadarAxis> axes);
    void setReference(std::vector<float> values);
    void paint(juce::Graphics& g) override;
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseExit(const juce::MouseEvent& e) override;

  private:
    std::vector<RadarAxis> axes_;
    std::vector<float> reference_;
    int hoveredAxis_ = -1;
    juce::Point<float> mousePos_; // last known mouse pos, updated in mouseMove
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RadarChartComponent)
};

} // namespace khensu
