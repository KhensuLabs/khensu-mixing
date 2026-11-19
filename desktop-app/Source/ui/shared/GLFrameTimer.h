// desktop-app/Source/ui/shared/GLFrameTimer.h
#pragma once
#include <JuceHeader.h>
#include <deque>

namespace khensu {

// Wall-clock rolling-average frame timer for OpenGL render calls. Deliberately
// CPU-side (not GL_TIME_ELAPSED query objects) - this targets a #version 120
// fixed/legacy GL context where query-object driver support is inconsistent,
// and wall-clock around renderOpenGL() is accurate enough to catch the
// "heavy analysis load" regressions this exists to profile.
class GLFrameTimer {
  public:
    void beginFrame() { frameStartMs_ = juce::Time::getMillisecondCounterHiRes(); }

    void endFrame()
    {
        const double elapsed = juce::Time::getMillisecondCounterHiRes() - frameStartMs_;
        samples_.push_back(elapsed);
        if (samples_.size() > kMaxSamples) samples_.pop_front();
    }

    double getAverageFrameMs() const
    {
        if (samples_.empty()) return 0.0;
        double sum = 0.0;
        for (double s : samples_)
            sum += s;
        return sum / (double)samples_.size();
    }

    double getPeakFrameMs() const
    {
        double peak = 0.0;
        for (double s : samples_)
            peak = juce::jmax(peak, s);
        return peak;
    }

    double getAverageFps() const
    {
        const double avg = getAverageFrameMs();
        return avg > 0.0 ? 1000.0 / avg : 0.0;
    }

  private:
    static constexpr size_t kMaxSamples = 120; // ~2s at 60fps
    double frameStartMs_ = 0.0;
    std::deque<double> samples_;
};

} // namespace khensu
