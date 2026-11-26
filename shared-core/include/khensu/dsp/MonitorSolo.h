// shared-core/include/khensu/dsp/MonitorSolo.h
// ENH-10 (2) / STU-5: monitoring on the output - Mono sum, Side only, Sub only
// (below 120 Hz, 4th-order Linkwitz-Riley) and a Low / Mid / High band solo
// (crossovers 250 Hz and 4 kHz, Linkwitz-Riley), plus Dim (-20 dB). Switching
// modes or dim crossfades over 15 ms, so no click.
//
// Shared by the plugin (the Solo menu) and the desktop transport (Mono /
// Side / Sub / Dim), so both hear the same crossovers.
//
// A monitoring aid, not a mix move: it is NOT a host parameter (no automation
// lane can switch it by accident), it is not saved with the project, and an
// offline render never hears it (the plugin passes `offline`; the desktop
// applies it only on the playback device's output, never to a render).
// THREAD_CONTEXT: setMode / setDim any thread; process audio thread (no
// allocation, no locks); prepare not concurrent with process.
#pragma once

#include <juce_dsp/juce_dsp.h>

#include <atomic>

namespace khensu::dsp {

class MonitorSolo {
  public:
    enum class Mode { Off = 0, Mono, Side, Sub, Low, Mid, High };
    static constexpr int kNumModes = 7;
    static constexpr float kSubHz = 120.0f;
    static constexpr float kLowMidHz = 250.0f;
    static constexpr float kMidHighHz = 4000.0f;
    static constexpr double kFadeSeconds = 0.015;
    /// STU-5: the Dim attenuation.
    static constexpr float kDimDb = -20.0f;

    /// Button / menu text of a mode ("Mono", "Sub < 120 Hz", ...).
    static juce::String modeName(Mode mode);

    void prepare(double sampleRate, int maxBlockSize);
    void setMode(Mode mode) noexcept { requested_.store(static_cast<int>(mode), std::memory_order_relaxed); }
    Mode mode() const noexcept { return static_cast<Mode>(requested_.load(std::memory_order_relaxed)); }
    /// STU-5: Dim the output by kDimDb (ramped over kFadeSeconds).
    void setDim(bool dim) noexcept { dimRequested_.store(dim, std::memory_order_relaxed); }
    bool dim() const noexcept { return dimRequested_.load(std::memory_order_relaxed); }
    /// True while the output is anything but the plain signal (a mode or dim
    /// on, or fading out of one).
    bool isActive() const noexcept { return active_.load(std::memory_order_relaxed); }

    /// AUDIO THREAD. Up to two channels; with `offline` the plain signal
    /// passes (and a running mode fades out).
    void process(juce::AudioBuffer<float>& buffer, bool offline) noexcept;
    /// AUDIO THREAD. The same on `numSamples` of a buffer starting at
    /// `startSample` (an AudioSource's channel info).
    void process(juce::AudioBuffer<float>& buffer, int startSample, int numSamples, bool offline) noexcept;

  private:
    float valueFor(Mode mode, int channel, float left, float right) const noexcept;

    using Filter = juce::dsp::LinkwitzRileyFilter<float>;
    Filter subSplit_, lowSplit_, highSplit_; ///< 120 Hz; 250 Hz; 4 kHz (on the part above 250 Hz)
    // Per-sample band outputs of the current sample (filled once per sample).
    float sub_[2]{}, low_[2]{}, mid_[2]{}, high_[2]{};

    std::atomic<int> requested_{0};
    std::atomic<bool> dimRequested_{false};
    std::atomic<bool> active_{false};
    Mode from_ = Mode::Off, to_ = Mode::Off;
    int fadePos_ = 0, fadeSamples_ = 720;
    float dimGain_ = 1.0f; ///< audio thread: the dim gain reached
    float dimStep_ = 1.0f / 720.0f;
    bool filtersWarm_ = false;
};

} // namespace khensu::dsp
