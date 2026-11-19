// desktop-app/Source/ui/shared/LiveLufsMeterComponent.h
// DSK-15: the values come from the DAW plugin's telemetry and are an RMS-EMA
// ESTIMATE, not K-weighted BS.1770 loudness - so the meter labels them
// "RMS est." and never calls them LUFS. It is fed only while the Copilot
// plugin plays in a DAW (the desktop transport does not feed it), and
// gated/silent readings never enter the histogram.
// Live momentary + short-term LUFS bar meters, plus a rolling distribution
// histogram of momentary values seen during playback - the classic
// mastering-meter view (Youlean/TC Electronic style) that a single "LIVE ST
// -14.2 M -12.1" text readout can't show: how *consistently* loud the
// playback has been, not just its current instantaneous value.
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include <array>
#include <cmath>

namespace khensu {

class LiveLufsMeterComponent final : public juce::Component {
  public:
    LiveLufsMeterComponent();

    /// Push a new (momentary, short-term) LUFS reading - call from the
    /// real-time meter listener during playback.
    void pushReading(float momentaryLufs, float shortTermLufs);

    /// Clears the histogram (call on new session/analysis so a previous
    /// track's distribution doesn't bleed into the next).
    void reset();

    void paint(juce::Graphics& g) override;

    /// Silent / gated readings (below the BS.1770 absolute gate, or not a
    /// number) do not count toward the distribution.
    static bool isGatedReading(float value) noexcept { return !std::isfinite(value) || value <= -70.0f; }
    int histogramTotal() const noexcept
    {
        int n = 0;
        for (int v : histogram_)
            n += v;
        return n;
    }

  private:
    static constexpr float kMin = -40.0f;
    static constexpr float kMax = 0.0f;
    static constexpr int kBins = 40; // 1 LU per bin

    float momentary_ = kMin;
    float shortTerm_ = kMin;
    bool hasReading_ = false;

    std::array<int, kBins> histogram_{};
    int histogramMax_ = 1;

    int binFor(float lufs) const;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LiveLufsMeterComponent)
};

} // namespace khensu
