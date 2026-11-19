#include "CopilotLiveAnalyzer.h"

namespace khensu::live {

CopilotLiveAnalyzer::CopilotLiveAnalyzer(timeline::IssueTimeline& timeline, double windowSec)
    : juce::Thread("khensu-live-analysis"), timeline_(timeline),
      scanner_(timeline::IssueSource::Live, "live", windowSec)
{
}

CopilotLiveAnalyzer::~CopilotLiveAnalyzer() { stop(); }

void CopilotLiveAnalyzer::start()
{
    if (!isThreadRunning()) startThread(juce::Thread::Priority::low);
}

void CopilotLiveAnalyzer::stop() { stopThread(4000); }

bool CopilotLiveAnalyzer::pushBlock(const float* const* channels, int numChannels, int numSamples,
                                    double hostTimeSec, bool hostTimeValid, bool isPlaying,
                                    double sampleRate) noexcept
{
    const bool playing = isPlaying && hostTimeValid && enabled_.load(std::memory_order_relaxed);
    // Transport stopped: the worker measures the partial window it holds.
    if (wasPlaying_.exchange(playing, std::memory_order_relaxed) && !playing)
        flushRequested_.store(true, std::memory_order_relaxed);
    if (!playing) return true;
    lastQueuedSec_.store(hostTimeSec, std::memory_order_relaxed);
    return fifo_.push(channels, numChannels, numSamples, hostTimeSec, sampleRate);
}

int CopilotLiveAnalyzer::drainOnce()
{
    int published = 0;
    const auto sink = [this, &published](WindowIssueScanner::Window&& window) {
        timeline_.replaceWindow(timeline::IssueSource::Live, window.startSec, window.endSec,
                                std::move(window.issues));
        windowsMeasured_.fetch_add(1, std::memory_order_relaxed);
        ++published;
    };
    auto& slot = *scratch_;
    // Bounded per call so a stop request is seen promptly.
    for (int i = 0; i < fifo_.capacity() && fifo_.pop(slot); ++i) {
        const float* ptrs[LiveAnalysisFifo::kMaxChannels] = {slot.data[0].data(), slot.data[1].data()};
        scanner_.feed(ptrs, slot.numChannels, slot.numSamples, slot.timeSec, slot.sampleRate, sink);
    }
    if (fifo_.numReady() == 0 && flushRequested_.exchange(false, std::memory_order_relaxed)) scanner_.flush(sink);
    return published;
}

void CopilotLiveAnalyzer::run()
{
    while (!threadShouldExit()) {
        wait(50);
        if (threadShouldExit()) return;
        if (!enabled_.load(std::memory_order_relaxed)) {
            // Drain and discard so the FIFO does not hold stale audio.
            while (fifo_.pop(*scratch_)) {
            }
            scanner_.reset();
            continue;
        }
        drainOnce();
    }
}

} // namespace khensu::live
