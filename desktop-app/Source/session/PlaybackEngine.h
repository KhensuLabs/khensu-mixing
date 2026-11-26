// desktop-app/Source/session/PlaybackEngine.h
#pragma once
#include <JuceHeader.h>
#include <atomic>
#include <memory>
#include "PlaybackMeterTap.h"
#include "LivePreviewStage.h"
#include <khensu/dsp/MonitorSolo.h>

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
/// read-ahead buffer at the new position and fades back in, so an undo/redo
/// or render switch is a short dip instead of a hard cut followed by an
/// empty-buffer gap.
///
/// A/B (UI-6): with an alternate file loaded beside the main one, both play
/// in lock-step (the mixer aligns the alternate to the main deck's read
/// position on the audio thread) and crossfadeTo() moves between them with an
/// equal-power crossfade over kCrossfadeMs - no fade-out, reload and fade-in,
/// and no level dip in the middle.
///
/// The chain, source to device:
///   decks -> AbMixer (A/B, matched gains) -> LivePreviewStage (STU-9: a
///   chain heard while it is edited) -> FadeGate (+ the meter tap) ->
///   MonitorStage (STU-5: Mono / Side / Sub / Dim) -> device.
/// Monitoring comes after the A/B match and after the meters (they keep
/// reading the program, not the monitoring), and only on this device path:
/// renders and exports are written by the render services from files and
/// never pass through it.
class PlaybackEngine {
  public:
    /// Length of the fade-out / fade-in around a source switch, start or stop.
    static constexpr double kFadeMs = 15.0;
    /// UI-6: the A <-> B crossfade (40-60 ms: short enough to compare, long
    /// enough to be click-free).
    static constexpr double kCrossfadeMs = 50.0;

    PlaybackEngine();
    ~PlaybackEngine();

    void play();  ///< pre-roll + fade in
    void pause(); ///< fade out, stop, keep position
    void stop();  ///< fade out, stop and rewind to 0
    void setPosition(double seconds);
    double getPosition() const;
    double getLength() const;
    bool isPlaying() const;
    void setGain(float linear); ///< 0..1 applied to the main deck

    /// Load a file as the transport source. Returns the reader sample rate
    /// (> 0) on success, 0.0 on failure (missing/undecodable). Detaches any
    /// previous source - and the alternate - first (fading out if playing).
    double loadFile(const juce::File& file);
    void clearSource(); ///< fade out (if playing) and detach both decks

    // ---- A/B (UI-6) ----
    /// Loads `file` as the alternate deck beside the main one, at the main
    /// deck's position (and playing if it is). The main deck stays audible.
    /// Returns the reader sample rate, 0.0 on failure.
    double loadAlternate(const juce::File& file);
    /// Detaches the alternate deck (crossfading back to the main one first).
    void clearAlternate();
    bool hasAlternate() const noexcept { return altLoaded_.load(std::memory_order_acquire); }
    /// Equal-power crossfade to the alternate (true) or the main deck.
    /// Immediate while nothing is audible.
    void crossfadeTo(bool alternate);
    bool alternateSelected() const noexcept { return mixer_.target() >= 0.5f; }
    void setAlternateGain(float linear); ///< 0..1 applied to the alternate deck

    /// UI-5: what the device plays, measured on the audio thread (lock-free).
    PlaybackMeterTap& meterTap() noexcept { return meterTap_; }

    // ---- STU-5: monitoring (any thread; the device output only) ----
    void setMonitorMode(dsp::MonitorSolo::Mode mode) noexcept { monitorStage_.solo().setMode(mode); }
    dsp::MonitorSolo::Mode monitorMode() const noexcept { return monitorStage_.solo().mode(); }
    void setMonitorDim(bool dim) noexcept { monitorStage_.solo().setDim(dim); }
    bool monitorDim() const noexcept { return monitorStage_.solo().dim(); }

    // ---- STU-9: live preview (message thread) ----
    LivePreviewStage& livePreview() noexcept { return livePreview_; }
    const LivePreviewStage& livePreview() const noexcept { return livePreview_; }

    /// Opens an independent reader for the file (caller owns / deletes it) —
    /// used for off-thread waveform-peak scanning without disturbing playback.
    juce::AudioFormatReader* createReaderFor(const juce::File& file);

    /// The equal-power law: at crossfade position t (0 = all A, 1 = all B),
    /// gainA = cos(t * pi/2), gainB = sin(t * pi/2), so gainA^2 + gainB^2 = 1
    /// - the summed power, and so the loudness, stays constant through the
    /// fade for uncorrelated material. Pure; unit-tested.
    static void equalPowerGains(float t, float& gainA, float& gainB) noexcept;

    /// Per-sample gain gate between the transport and the device. Ramps
    /// linearly toward a target set from the message thread; the audio thread
    /// publishes the gain it reached so the message thread can wait for a
    /// fade-out to land before cutting the source.
    class FadeGate final : public juce::AudioSource {
      public:
        FadeGate(juce::AudioSource& input, PlaybackMeterTap* tap = nullptr) : input_(input), tap_(tap) {}
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
        PlaybackMeterTap* tap_ = nullptr;
        std::atomic<float> target_{1.0f};
        std::atomic<float> reached_{1.0f};
        std::atomic<bool> snapRequested_{false};
        std::atomic<double> sampleRate_{0.0};
        float current_ = 1.0f; // audio thread only
    };

    /// Mixes the main and alternate decks with the equal-power law. The
    /// alternate is always read when loaded (so it keeps time with the main
    /// deck), and re-aligned to the main deck's read position when asked.
    /// No allocation or locks of its own on the audio thread: the scratch
    /// buffer is sized in prepareToPlay() and longer blocks are mixed in parts.
    class AbMixer final : public juce::AudioSource {
      public:
        AbMixer(juce::AudioTransportSource& main, juce::AudioTransportSource& alt) : main_(main), alt_(alt) {}
        void prepareToPlay(int samplesPerBlockExpected, double sampleRate) override;
        void releaseResources() override;
        void getNextAudioBlock(const juce::AudioSourceChannelInfo& info) override;

        void setAltActive(bool active) noexcept { altActive_.store(active, std::memory_order_release); }
        void crossfadeTo(float t) noexcept { target_.store(t, std::memory_order_release); }
        void snapTo(float t) noexcept
        {
            target_.store(t, std::memory_order_release);
            snap_.store(true, std::memory_order_release);
        }
        void requestResync() noexcept { resync_.store(true, std::memory_order_release); }
        float target() const noexcept { return target_.load(std::memory_order_acquire); }
        float reached() const noexcept { return reached_.load(std::memory_order_acquire); }

      private:
        juce::AudioTransportSource& main_;
        juce::AudioTransportSource& alt_;
        juce::AudioBuffer<float> scratch_;
        std::atomic<bool> altActive_{false};
        std::atomic<float> target_{0.0f};
        std::atomic<float> reached_{0.0f};
        std::atomic<bool> snap_{false};
        std::atomic<bool> resync_{false};
        double sampleRate_ = 44100.0;
        float t_ = 0.0f; // audio thread only
    };

    /// STU-5: the monitoring stage at the end of the chain (after the meter
    /// tap). Playback only - see the chain above.
    class MonitorStage final : public juce::AudioSource {
      public:
        explicit MonitorStage(juce::AudioSource& input) : input_(input) {}
        void prepareToPlay(int samplesPerBlockExpected, double sampleRate) override
        {
            input_.prepareToPlay(samplesPerBlockExpected, sampleRate);
            solo_.prepare(sampleRate, samplesPerBlockExpected);
        }
        void releaseResources() override { input_.releaseResources(); }
        void getNextAudioBlock(const juce::AudioSourceChannelInfo& info) override
        {
            input_.getNextAudioBlock(info);
            if (info.buffer != nullptr) solo_.process(*info.buffer, info.startSample, info.numSamples, false);
        }
        dsp::MonitorSolo& solo() noexcept { return solo_; }
        const dsp::MonitorSolo& solo() const noexcept { return solo_; }

      private:
        juce::AudioSource& input_;
        dsp::MonitorSolo solo_;
    };

  private:
    /// True when an audio device is open and calling back, i.e. a fade can
    /// actually progress (otherwise waiting for one would only stall).
    bool deviceRunning() const;
    /// Fades the gate to silence and waits (bounded) for the audio thread to
    /// get there. No-op when nothing is playing.
    void fadeOutAndWait();
    void detachAlternate();

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
    juce::AudioTransportSource altTransport_;
    AbMixer mixer_{transportSource_, altTransport_};
    PlaybackMeterTap meterTap_;
    LivePreviewStage livePreview_{mixer_};
    FadeGate fadeGate_{livePreview_, &meterTap_};
    MonitorStage monitorStage_{fadeGate_};
    juce::AudioFormatManager formatManager_;
    std::unique_ptr<juce::AudioFormatReaderSource> readerSource_;
    // Owned here (not by the transport) so play() can wait on it; declared
    // after readerSource_ so it is destroyed first.
    std::unique_ptr<juce::BufferingAudioSource> bufferingSource_;
    std::unique_ptr<juce::AudioFormatReaderSource> altReaderSource_;
    std::unique_ptr<juce::BufferingAudioSource> altBufferingSource_;
    std::atomic<bool> altLoaded_{false};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PlaybackEngine)
};

} // namespace khensu
