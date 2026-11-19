#pragma once

#include <JuceHeader.h>
#include "KhensuTheme.h"

namespace khensu {

/// Compact radial readout of a measured value's distance from one explicit
/// genre target. The ring fill represents closeness, while the centre keeps
/// the signed-unit context out of the visual encoding.
class TargetRingGauge final : public juce::Component, public juce::SettableTooltipClient {
  public:
    TargetRingGauge() = default;
    void setTarget(const juce::String& label, float value, float target, float tolerance, const juce::String& unit);

    /// 1 = exact target, 0 = at least three tolerance widths away.
    static float matchScore(float value, float target, float tolerance) noexcept;

    void paint(juce::Graphics& g) override;

  private:
    juce::String label_;
    juce::String unit_;
    float value_ = 0.0f;
    float target_ = 0.0f;
    float tolerance_ = 1.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TargetRingGauge)
};

} // namespace khensu
