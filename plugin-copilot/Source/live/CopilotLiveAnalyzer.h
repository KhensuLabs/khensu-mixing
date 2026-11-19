#pragma once

#include <JuceHeader.h>
#include "LiveAnalysisFifo.h"
#include "WindowIssueScanner.h"
#include "../timeline/IssueTimeline.h"
#include <atomic>
#include <memory>

namespace khensu::live {

/// Rolling analysis of the audio the plugin outputs, stamped with the host
/// PlayHead position (CE-DAW-3).
///
///   audio thread : pushBlock() - copies into the preallocated lock-free FIFO
///                  (no allocation, no lock), only while the host is playing
///                  and reports a timeline position;
///   worker thread: drains the FIFO into a WindowIssueScanner and publishes
///                  each measured window to the IssueTimeline, replacing what
///                  was measured at that time before.
///
/// The worker is owned and joined in the destructor (no detached threads).
class CopilotLiveAnalyzer final : private juce::Thread {
  public:
    explicit CopilotLiveAnalyzer(timeline::IssueTimeline& timeline, double windowSec = 4.0);
    ~CopilotLiveAnalyzer() override;

    /// THREAD_CONTEXT(UIThread) - starts the worker on first use.
    void start();
    /// THREAD_CONTEXT(UIThread) - joins the worker (bounded).
    void stop();

    /// THREAD_CONTEXT(AudioThread). `hostTimeSec` is the timeline position of
    /// the block's first sample; blocks without a position or while stopped
    /// are ignored. Returns false if the FIFO had to drop audio.
    bool pushBlock(const float* const* channels, int numChannels, int numSamples, double hostTimeSec,
                   bool hostTimeValid, bool isPlaying, double sampleRate) noexcept;

    void setEnabled(bool enabled) noexcept { enabled_.store(enabled, std::memory_order_relaxed); }
    bool isEnabled() const noexcept { return enabled_.load(std::memory_order_relaxed); }

    /// Worker side: drains the FIFO once (the worker loop calls this; tests
    /// call it directly to stay deterministic). Returns windows published.
    int drainOnce();

    juce::int64 droppedSlots() const noexcept { return fifo_.droppedSlots(); }
    int windowsMeasured() const noexcept { return windowsMeasured_.load(std::memory_order_relaxed); }
    /// Host time of the most recent block handed to the analysis.
    double lastQueuedTimeSec() const noexcept { return lastQueuedSec_.load(std::memory_order_relaxed); }

  private:
    void run() override;

    timeline::IssueTimeline& timeline_;
    LiveAnalysisFifo fifo_;
    WindowIssueScanner scanner_; ///< worker-only
    std::unique_ptr<LiveAnalysisFifo::Slot> scratch_{std::make_unique<LiveAnalysisFifo::Slot>()}; ///< worker-only
    std::atomic<bool> enabled_{true};
    std::atomic<bool> wasPlaying_{false};
    std::atomic<bool> flushRequested_{false};
    std::atomic<int> windowsMeasured_{0};
    std::atomic<double> lastQueuedSec_{0.0};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CopilotLiveAnalyzer)
};

} // namespace khensu::live
