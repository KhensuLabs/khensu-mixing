// plugin-copilot/Source/GrowableDelayLine.h
// FIX-14: a dry-compensation delay line whose capacity can grow while audio
// runs. The control thread allocates a bigger line and posts it; the audio
// thread adopts it at a block boundary and hands the old one back; the
// control thread frees it. The audio thread never allocates, frees or locks.
#pragma once

#include <JuceHeader.h>

#include <atomic>
#include <memory>

namespace khensu::copilot {

class GrowableDelayLine {
  public:
    using Line = juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::None>;

    GrowableDelayLine() = default;
    ~GrowableDelayLine() { releaseAll(); }
    GrowableDelayLine(const GrowableDelayLine&) = delete;
    GrowableDelayLine& operator=(const GrowableDelayLine&) = delete;

    /// Not concurrent with process(): (re)allocates the active line with room
    /// for `capacitySamples` of delay and drops anything pending.
    void prepare(double sampleRate, int blockSize, int channels, int capacitySamples)
    {
        const juce::SpinLock::ScopedLockType sl(controlLock_);
        sampleRate_ = sampleRate;
        blockSize_ = juce::jmax(1, blockSize);
        channels_ = juce::jmax(1, channels);
        releaseAll();
        active_ = makeLine(juce::jmax(1, capacitySamples)).release();
        reserved_.store(active_->capacity, std::memory_order_release);
    }

    /// Control thread: makes sure a line with room for `samples` of delay
    /// exists or is on its way. Allocates only when growing. Returns the
    /// capacity the line will have once the audio thread adopts it.
    int reserve(int samples)
    {
        const juce::SpinLock::ScopedLockType sl(controlLock_);
        reapRetiredLocked();
        if (active_ == nullptr || samples <= reserved_.load(std::memory_order_acquire))
            return reserved_.load(std::memory_order_acquire);
        // Headroom so a plugin creeping up by a few samples doesn't realloc.
        const int capacity = samples + static_cast<int>(std::ceil(sampleRate_ * 0.25));
        auto line = makeLine(capacity);
        delete pending_.exchange(line.release(), std::memory_order_acq_rel);
        reserved_.store(capacity, std::memory_order_release);
        return capacity;
    }

    /// Control thread: frees a line the audio thread has handed back.
    void reapRetired()
    {
        const juce::SpinLock::ScopedLockType sl(controlLock_);
        reapRetiredLocked();
    }

    /// Capacity reserved so far: what latency reporting may rely on.
    int reservedCapacity() const noexcept { return reserved_.load(std::memory_order_acquire); }

    // ---- audio thread ------------------------------------------------------

    /// Adopts a pending bigger line, if any. Call once at the start of a
    /// block. The new line starts silent: the dry side is re-aligned anyway
    /// whenever the latency it compensates changes.
    void adoptPending() noexcept
    {
        if (retired_.load(std::memory_order_acquire) != nullptr) return; // previous swap not reaped yet
        if (auto* next = pending_.exchange(nullptr, std::memory_order_acq_rel)) {
            retired_.store(active_, std::memory_order_release);
            active_ = next;
        }
    }

    /// Capacity of the line the audio thread is using now.
    int activeCapacity() const noexcept { return active_ != nullptr ? active_->capacity : 0; }

    /// Delays `samples` in place on `channel` by `delay` samples (clamped to
    /// the active capacity).
    void process(int channel, float* samples, int numSamples, int delay) noexcept
    {
        if (active_ == nullptr || channel >= channels_) return;
        const auto d = static_cast<float>(juce::jlimit(0, active_->capacity, delay));
        for (int i = 0; i < numSamples; ++i) {
            active_->line.pushSample(channel, samples[i]);
            samples[i] = active_->line.popSample(channel, d, true);
        }
    }

    void reset() noexcept
    {
        if (active_ != nullptr) active_->line.reset();
    }

  private:
    struct Box {
        Line line{1};
        int capacity = 0;
    };

    std::unique_ptr<Box> makeLine(int capacity) const
    {
        auto box = std::make_unique<Box>();
        box->capacity = capacity;
        box->line.setMaximumDelayInSamples(capacity);
        box->line.prepare({sampleRate_, static_cast<juce::uint32>(blockSize_), static_cast<juce::uint32>(channels_)});
        box->line.reset();
        return box;
    }

    void reapRetiredLocked() { delete retired_.exchange(nullptr, std::memory_order_acq_rel); }

    void releaseAll()
    {
        delete pending_.exchange(nullptr, std::memory_order_acq_rel);
        delete retired_.exchange(nullptr, std::memory_order_acq_rel);
        delete active_;
        active_ = nullptr;
        reserved_.store(0, std::memory_order_release);
    }

    juce::SpinLock controlLock_; // control threads only
    double sampleRate_ = 44100.0;
    int blockSize_ = 512;
    int channels_ = 2;
    Box* active_ = nullptr; // audio thread (and prepare)
    std::atomic<Box*> pending_{nullptr};
    std::atomic<Box*> retired_{nullptr};
    std::atomic<int> reserved_{0};
};

} // namespace khensu::copilot
