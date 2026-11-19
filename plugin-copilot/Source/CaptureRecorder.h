// plugin-copilot/Source/CaptureRecorder.h
// FEAT-7: records what the host sends the plugin - the next N bars of
// playback, or the loop region - for a capture to the desktop app. The
// buffer is allocated when the capture is armed (message thread); the audio
// thread only copies into it, with no allocation or lock.
#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>

namespace khensu::copilot {

class CaptureRecorder {
  public:
    enum class State : int {
        Idle = 0,
        Armed, ///< waiting for the host to play
        Recording,
        Done,       ///< the buffer holds recordedSamples() samples
        Cancelling, ///< asked to stop; the audio thread sets Idle
    };

    /// THREAD_CONTEXT(UIThread). Allocates `samples` x `channels` and waits for
    /// playback; false while a capture is armed or recording, or for 0 samples.
    bool arm(juce::int64 samples, int channels)
    {
        const auto s = state();
        if (samples <= 0 || channels <= 0 || (s != State::Idle && s != State::Done)) return false;
        buffer_.setSize(channels, static_cast<int>(juce::jmin<juce::int64>(samples, 1 << 30)), false, true, false);
        target_ = buffer_.getNumSamples();
        written_.store(0, std::memory_order_relaxed);
        state_.store(static_cast<int>(State::Armed), std::memory_order_release);
        return true;
    }

    /// THREAD_CONTEXT(UIThread). Stops an armed or running capture.
    void cancel()
    {
        int armed = static_cast<int>(State::Armed);
        if (state_.compare_exchange_strong(armed, static_cast<int>(State::Idle))) return;
        int recording = static_cast<int>(State::Recording);
        state_.compare_exchange_strong(recording, static_cast<int>(State::Cancelling));
    }

    /// THREAD_CONTEXT(UIThread). Back to Idle after a finished capture was written.
    void reset()
    {
        int done = static_cast<int>(State::Done);
        state_.compare_exchange_strong(done, static_cast<int>(State::Idle));
    }

    /// THREAD_CONTEXT(AudioThread). Starts on playback; ends when full, or
    /// when playback stops with something recorded.
    void process(const juce::AudioBuffer<float>& block, bool hostPlaying) noexcept
    {
        int s = state_.load(std::memory_order_acquire);
        if (s == static_cast<int>(State::Cancelling)) {
            state_.store(static_cast<int>(State::Idle), std::memory_order_release);
            return;
        }
        if (s == static_cast<int>(State::Armed)) {
            if (!hostPlaying) return;
            if (!state_.compare_exchange_strong(s, static_cast<int>(State::Recording))) return;
            s = static_cast<int>(State::Recording);
        }
        if (s != static_cast<int>(State::Recording)) return;
        const auto written = written_.load(std::memory_order_relaxed);
        if (!hostPlaying) {
            state_.store(static_cast<int>(written > 0 ? State::Done : State::Armed), std::memory_order_release);
            return;
        }
        const int n = static_cast<int>(juce::jmin<juce::int64>(block.getNumSamples(), target_ - written));
        const int channels = buffer_.getNumChannels();
        for (int ch = 0; ch < channels && n > 0; ++ch) {
            // A mono input fills every channel of the capture.
            const int src = juce::jmin(ch, block.getNumChannels() - 1);
            if (src >= 0) buffer_.copyFrom(ch, static_cast<int>(written), block, src, 0, n);
        }
        written_.store(written + n, std::memory_order_relaxed);
        if (written + n >= target_) state_.store(static_cast<int>(State::Done), std::memory_order_release);
    }

    State state() const noexcept { return static_cast<State>(state_.load(std::memory_order_acquire)); }
    /// 0..1 of the capture recorded.
    double progress() const noexcept
    {
        return target_ > 0
                   ? static_cast<double>(written_.load(std::memory_order_relaxed)) / static_cast<double>(target_)
                   : 0.0;
    }
    juce::int64 recordedSamples() const noexcept { return written_.load(std::memory_order_relaxed); }
    /// THREAD_CONTEXT(UIThread). The recording, valid in state Done.
    const juce::AudioBuffer<float>& buffer() const noexcept { return buffer_; }

  private:
    juce::AudioBuffer<float> buffer_;
    juce::int64 target_ = 0;
    std::atomic<juce::int64> written_{0};
    std::atomic<int> state_{static_cast<int>(State::Idle)};
};

} // namespace khensu::copilot
