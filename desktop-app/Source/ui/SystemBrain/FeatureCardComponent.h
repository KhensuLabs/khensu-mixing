// desktop-app/Source/ui/SystemBrain/FeatureCardComponent.h
#pragma once
#include <JuceHeader.h>
#include <khensu/systembrain/FeatureEntry.h>
#include <functional>

namespace khensu {

// THREAD_CONTEXT(UIThread)
class FeatureCardComponent final : public juce::Component {
  public:
    FeatureCardComponent() = default;
    void setEntry(const FeatureEntry& e);
    int preferredHeight() const { return 76; }

    void setSessionValue(float val, float minVal, float maxVal, float lo, float hi)
    {
        hasSessionValue_ = true;
        sessionValue_ = val;
        rangeMin_ = minVal;
        rangeMax_ = maxVal;
        zoneLo_ = lo;
        zoneHi_ = hi;
        repaint();
    }

    std::function<void(const std::string& id)> onSelect;

    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;

  private:
    void drawBarVisualization(juce::Graphics& g, const juce::Rectangle<int>& area);
    void drawDotVisualization(juce::Graphics& g, const juce::Rectangle<int>& area);
    void drawPercentileVisualization(juce::Graphics& g, const juce::Rectangle<int>& area);
    void drawPresenceVisualization(juce::Graphics& g, const juce::Rectangle<int>& area);
    void drawIconVisualization(juce::Graphics& g, const juce::Rectangle<int>& area);

    float normaliseValue(float v) const;
    bool valueInTargetZone() const;
    juce::Colour valueColour() const;

    FeatureEntry entry_;
    bool hasSessionValue_ = false;
    float sessionValue_ = 0.0f;
    float rangeMin_ = 0.0f;
    float rangeMax_ = 1.0f;
    float zoneLo_ = 0.0f;
    float zoneHi_ = 1.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FeatureCardComponent)
};

} // namespace khensu
