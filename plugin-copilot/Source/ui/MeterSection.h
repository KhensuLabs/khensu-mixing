// plugin-copilot/Source/ui/MeterSection.h
#pragma once
#include <JuceHeader.h>

namespace khensu {
class KhensuCopilotProcessor;

// Live L/R VU meter panel. Updates at ~30fps via juce::Timer.
// THREAD_CONTEXT(UIThread)
class MeterSection final : public juce::Component, private juce::Timer {
  public:
    explicit MeterSection(KhensuCopilotProcessor& p);
    ~MeterSection() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

  private:
    KhensuCopilotProcessor& processor_;

    float leftDb_ = -96.0f;
    float rightDb_ = -96.0f;
    float leftHold_ = -96.0f;
    float rightHold_ = -96.0f;
    int holdCounterL_ = 0;
    int holdCounterR_ = 0;

    static constexpr float kFloorDb = -60.0f;
    static constexpr float kCeilDb = 0.0f;
    static constexpr int kHoldFrames = 20;

    void timerCallback() override;
    void drawBar(juce::Graphics& g, juce::Rectangle<int> bounds, float levelDb, float holdDb, bool isLeft) const;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MeterSection)
};

} // namespace khensu
