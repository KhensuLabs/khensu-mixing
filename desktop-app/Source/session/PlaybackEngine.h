// desktop-app/Source/session/PlaybackEngine.h
#pragma once
#include <JuceHeader.h>
#include <atomic>
#include <memory>

namespace khensu {

/// The audio playback pipeline extracted from AppController (audit M4). Owns the
/// JUCE device -> source-player -> transport chain and the current file reader,
/// and exposes the primitive transport operations. It knows nothing about A/B,
/// loudness matching, streaming preview, or waveform peaks — AppController keeps
/// that policy and drives this engine (deciding what file to load and what gain
/// to apply), so the audio device lifecycle lives in exactly one place.
///
/// Source switches are de-clicked (AUDIT-037): detaching a playing source
/// first fades the output out over kFadeMs, and play() pre-rolls the
/// read-ahead buffer at the new position and fades back in, so an A/B toggle,
/// undo/redo or render switch is a short dip instead of a hard cut followed
/// by an empty-buffer gap.
class PlaybackEngine {
  public:
    /// Length of the fade-out / fade-in around a source switch, start or stop.
    static constexpr double kFadeMs = 15.0;

    PlaybackEngine();
    ~PlaybackEngine();

    void play();  ///< pre-roll + fade in
    void pause(); ///< fade out, stop, keep position
    void stop();  ///< fade out, stop and rewind to 0
    void setPosition(double seconds);
    double getPosition() const;
    double getLength() const;
    bool isPlaying() const;
    void setGain(float linear); ///< 0..1 applied to the transport

    /// Load a file as the transport source. Returns the reader sample rate
    /// (> 0) on success, 0.0 on failure (missing/undecodable). Detaches any
    /// previous source first (fading it out if it was playing).
    double loadFile(const juce::File& file);
    void clearSource(); ///< fade out (if playing) and detach the current source

    /// Opens an independent reader for the file (caller owns / deletes it) —
    /// used for off-thread waveform-peak scanning without disturbing playback.
    juce::AudioFormatReader* createReaderFor(const juce::File& file);

    /// Per-sample gain gate between the transport and the device. Ramps
    /// linearly toward a target set from the message thread; the audio thread
    /// publishes the gain it reached so the message thread can wait for a
    /// fade-out to land before cutting the source.
    class FadeGate final : public juce::AudioSource {
      public:
        explicit FadeGate(juce::AudioSource& input) : input_(input) {}
        void prepareToPlay(int samplesPerBlockExpected, double sampleRate) override;
        void releaseResources() override { input_.releaseResources(); }
        void getNextAudioBlock(const juce::AudioSourceChannelInfo& info) override;

        void fadeTo(float target) noexcept { target_.store(target, std::memory_order_release); }
        /// Jump immediately (no ramp) - only safe while nothing is audible.
        void snapTo(float gain) noexcept
        {
            target_.store(gain, std::memory_order_release);
            snapRequested_.store(true, std::memory_order_release);
        }
        float reachedGain() const noexcept { return reached_.load(std::memory_order_acquire); }
        double sampleRate() const noexcept { return sampleRate_.load(std::memory_order_acquire); }

      private:
        juce::AudioSource& input_;
        std::atomic<float> target_{1.0f};
        std::atomic<float> reached_{1.0f};
        std::atomic<bool> snapRequested_{false};
        std::atomic<double> sampleRate_{0.0};
        float current_ = 1.0f; // audio thread only
    };

  private:

    /// True when an audio device is open and calling back, i.e. a fade can
    /// actually progress (otherwise waiting for one would only stall).
    bool deviceRunning() const;
    /// Fades the gate to silence and waits (bounded) for the audio thread to
    /// get there. No-op when nothing is playing.
    void fadeOutAndWait();

    /// Decodes ahead of the audio callback (kReadAheadSamples) so disk or
    /// sync-client stalls - this project often lives in a cloud-synced folder -
    /// don't starve the device and click. Declared first: destroyed last.
    juce::TimeSliceThread readAheadThread_{"Khensu playback read-ahead"};
    static constexpr int kReadAheadSamples = 65536;
    /// How much of the read-ahead buffer must be decoded at the play position
    /// before play() starts (pre-roll), and how long it may wait for that.
    static constexpr int kPrerollSamples = 8192;
    static constexpr juce::uint32 kPrerollTimeoutMs = 250;

    juce::AudioDeviceManager deviceManager_;
    juce::AudioSourcePlayer sourcePlayer_;
    juce::AudioTransportSource transportSource_;
    FadeGate fadeGate_{transportSource_};
    juce::AudioFormatManager formatManager_;
    std::unique_ptr<juce::AudioFormatReaderSource> readerSource_;
    // Owned here (not by the transport) so play() can wait on it; declared
    // after readerSource_ so it is destroyed first.
    std::unique_ptr<juce::BufferingAudioSource> bufferingSource_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PlaybackEngine)
};

} // namespace khensu
