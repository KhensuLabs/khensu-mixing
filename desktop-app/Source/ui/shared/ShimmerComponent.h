#pragma once
#include <JuceHeader.h>
#include "KhensuTheme.h"

namespace khensu {

class ShimmerComponent final : public juce::Component, private juce::Timer {
  public:
    ShimmerComponent() = default;

    void start();
    void stop();

    void paint(juce::Graphics& g) override;

  private:
    float phase_ = 0.0f; // 0..1, drives sweep position
    void timerCallback() override;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ShimmerComponent)
};

} // namespace khensu
