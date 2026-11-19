// desktop-app/Source/ui/shared/VoiceRecorder.h
#pragma once
#include <JuceHeader.h>
#include <atomic>
#include <vector>

namespace khensu {

/// CE-AG-4 push-to-talk capture: records the default input device into a
/// mono buffer. It owns its OWN AudioDeviceManager (input only, no outputs),
/// separate from PlaybackEngine's output-only manager, so opening and closing
/// the microphone never restarts or reconfigures playback.
///
/// start()/stop() on the message thread; the device callback only copies
/// into a buffer pre-allocated by start() (no allocation, no locks).
/// THREAD_CONTEXT(UIThread) except audioDeviceIOCallbackWithContext.
class VoiceRecorder final : private juce::AudioIODeviceCallback {
  public:
    VoiceRecorder() = default;
    ~VoiceRecorder() override;

    /// Opens the default input device and starts recording. Returns "" on
    /// success or a user-facing reason (no microphone, device error).
    juce::String start(double maxSeconds);
    /// Stops and closes the device; returns the recorded mono samples.
    std::vector<float> stop();
    bool isRecording() const noexcept { return recording_; }
    double sampleRate() const noexcept { return sampleRate_; }
    /// Seconds captured so far (any thread).
    double recordedSeconds() const noexcept
    {
        return sampleRate_ > 0.0 ? (double)written_.load(std::memory_order_acquire) / sampleRate_ : 0.0;
    }
    /// Peak level of the most recent device block, 0..1 (any thread).
    float inputLevel() const noexcept { return level_.load(std::memory_order_relaxed); }

  private:
    void audioDeviceIOCallbackWithContext(const float* const* inputChannelData, int numInputChannels,
                                          float* const* outputChannelData, int numOutputChannels, int numSamples,
                                          const juce::AudioIODeviceCallbackContext& context) override;
    void audioDeviceAboutToStart(juce::AudioIODevice*) override {}
    void audioDeviceStopped() override {}

    std::unique_ptr<juce::AudioDeviceManager> deviceManager_;
    std::vector<float> buffer_;
    std::atomic<size_t> written_{0};
    std::atomic<float> level_{0.0f};
    double sampleRate_ = 0.0;
    bool recording_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VoiceRecorder)
};

} // namespace khensu
