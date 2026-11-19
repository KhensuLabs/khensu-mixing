#pragma once

#include <JuceHeader.h>
#include <khensu/analysis/AnalysisResult.h>
#include "../timeline/IssueTimeline.h"
#include <functional>
#include <string>
#include <vector>

namespace khensu::live {

/// Cuts a time-stamped audio stream into fixed analysis windows and measures
/// each one with the shared-core analyzers and recommenders (CE-DAW-3; the
/// ARA source analysis of CE-DAW-1 uses it too).
///
/// The stream may jump (the host transport looped, seeked or stopped, or the
/// FIFO dropped a block): feed() compares each block's time with the end of
/// the previous one and, on a gap, closes the current window early - it is
/// measured if it is long enough, else discarded - and starts a new window at
/// the new position. Every result is therefore stamped with the host time
/// range the audio actually came from.
///
/// THREAD_CONTEXT(WorkerThread) - allocates and runs FFTs; never call it from
/// the audio thread.
class WindowIssueScanner {
  public:
    struct Window {
        double startSec = 0.0;
        double endSec = 0.0;
        std::vector<timeline::TimedIssue> issues;
    };
    using WindowSink = std::function<void(Window&&)>;

    /// Gap tolerated between consecutive blocks before it counts as a jump.
    static constexpr double kJumpToleranceSec = 0.010;

    WindowIssueScanner(timeline::IssueSource source, std::string fileId, double windowSec = 4.0,
                       double minWindowSec = 1.5);

    /// Starts over (sample rate or channel count changed, analysis toggled).
    void reset();

    /// Feeds one contiguous block that starts at `timeSec` on the timeline.
    /// Completed windows are handed to `sink`.
    void feed(const float* const* channels, int numChannels, int numSamples, double timeSec, double sampleRate,
              const WindowSink& sink);

    /// Measures the partial window if it is at least minWindowSec long.
    void flush(const WindowSink& sink);

    /// Measures one buffer that starts at `startSec` (no windowing). Public so
    /// tests and the ARA job can call it directly.
    static AnalysisResult measure(const juce::AudioBuffer<float>& audio, double sampleRate,
                                  const std::string& fileId);
    static std::vector<timeline::TimedIssue> analyseWindow(const juce::AudioBuffer<float>& audio, int numSamples,
                                                           double sampleRate, double startSec,
                                                           timeline::IssueSource source, const std::string& fileId,
                                                           const std::string& regionName = {});

    double windowSec() const noexcept { return windowSec_; }
    /// Windows measured / discarded (too short when a jump closed them).
    int windowsMeasured() const noexcept { return measured_; }
    int windowsDiscarded() const noexcept { return discarded_; }
    void setRegionName(std::string name) { regionName_ = std::move(name); }

  private:
    void closeWindow(const WindowSink& sink, bool allowPartial);

    timeline::IssueSource source_;
    std::string fileId_;
    std::string regionName_;
    double windowSec_;
    double minWindowSec_;

    juce::AudioBuffer<float> window_;
    int filled_ = 0;
    int channels_ = 0;
    double sampleRate_ = 0.0;
    double windowStartSec_ = 0.0;
    double expectedNextSec_ = -1.0;
    int measured_ = 0;
    int discarded_ = 0;
};

} // namespace khensu::live
