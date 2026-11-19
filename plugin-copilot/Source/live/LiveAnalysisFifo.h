#pragma once

#include <JuceHeader.h>
#include <array>
#include <atomic>
#include <vector>

namespace khensu::live {

/// Lock-free, single-producer / single-consumer handoff of audio from the
/// audio thread to the live-analysis worker (CE-DAW-3).
///
/// Every slot carries up to kSlotSamples frames of at most kMaxChannels
/// channels and the HOST timeline position of its first frame. All storage is
/// allocated once in the constructor, so push() never allocates, locks or
/// blocks: when the worker falls behind and the FIFO is full the block is
/// dropped and counted (the worker then sees a time gap and restarts its
/// window, rather than measuring a spliced signal).
class LiveAnalysisFifo {
  public:
    static constexpr int kSlotSamples = 512;
    static constexpr int kMaxChannels = 2;

    struct Slot {
        double timeSec = 0.0;  ///< host timeline position of frame 0
        double sampleRate = 0.0;
        int numSamples = 0;
        int numChannels = 0;
        std::array<std::array<float, kSlotSamples>, kMaxChannels> data{};
    };

    explicit LiveAnalysisFifo(int numSlots = 192);

    /// THREAD_CONTEXT(AudioThread). Splits the block into slots stamped with
    /// timeSec + offset / sampleRate. Returns false if any part was dropped.
    bool push(const float* const* channels, int numChannels, int numSamples, double timeSec,
              double sampleRate) noexcept;

    /// THREAD_CONTEXT(WorkerThread). Copies the oldest slot into `out`.
    bool pop(Slot& out) noexcept;

    int numReady() const noexcept { return fifo_.getNumReady(); }
    int capacity() const noexcept { return static_cast<int>(slots_.size()); }
    /// Blocks (or block tails) dropped because the FIFO was full.
    juce::int64 droppedSlots() const noexcept { return dropped_.load(std::memory_order_relaxed); }

  private:
    std::vector<Slot> slots_;
    juce::AbstractFifo fifo_;
    std::atomic<juce::int64> dropped_{0};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LiveAnalysisFifo)
};

} // namespace khensu::live
