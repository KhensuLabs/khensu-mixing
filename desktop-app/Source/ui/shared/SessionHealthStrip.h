// desktop-app/Source/ui/shared/SessionHealthStrip.h
// Listening Fatigue Coach + Session Health HUD: an ambient strip above the
// transport tracking session length, time since the last real listening
// break, a monitoring-level-aware ear-fatigue estimate, and recent decision
// rate. The craft books all prescribe the same discipline - take breaks,
// check at low level, don't trust hour-three ears - this makes the app
// enforce what the mentors teach.
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>

namespace khensu {

class IAppController;

class SessionHealthStrip final : public juce::Component, private juce::Timer {
  public:
    explicit SessionHealthStrip(IAppController& c);
    ~SessionHealthStrip() override;

    void paint(juce::Graphics& g) override;

    /// Preferred height when visible.
    static constexpr int kHeight = 22;

  private:
    void timerCallback() override;

    IAppController& controller_;

    juce::int64 sessionStartMs_;
    juce::int64 lastBreakEndMs_;     // most recent moment a >=3 min pause ended
    juce::int64 pauseStartedMs_ = 0; // 0 = currently playing / no pause running
    int rendersAtLastTick_ = 0;
    float decisionsPer10Min_ = 0.0f;
    juce::int64 lastDecisionWindowMs_;
    int decisionsThisWindow_ = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SessionHealthStrip)
};

} // namespace khensu
