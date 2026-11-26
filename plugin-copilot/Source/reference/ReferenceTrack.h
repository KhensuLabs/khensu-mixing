// plugin-copilot/Source/reference/ReferenceTrack.h
// ENH-10 (1): a reference track inside the plugin.
//
//  - loadReference() decodes a file, resamples it to the host rate and
//    measures it (LoudnessStats + the Mix hub's band grid). It runs on a
//    worker (PluginWorkerPool), never the audio thread: like CaptureRecorder,
//    every allocation happens before the audio thread sees the data.
//  - ReferencePlayer hands the decoded audio to the audio thread through an
//    atomic pointer (adopted at a block boundary, the old holder handed back
//    and freed on the message thread - the GrowableDelayLine pattern), and
//    plays it loudness matched against the host signal: the reference is
//    turned by (host loudness - reference loudness), integrated or
//    short-term, smoothed, and crossfaded in and out over 15 ms.
//  - Offline renders never hear the reference (the processor passes
//    `offline`), the same rule as the A/B (PLG-12).
#pragma once

#include <JuceHeader.h>
#include "LoudnessStats.h"
#include "../mixhub/MixHubAnalysis.h"

#include <atomic>
#include <functional>
#include <memory>
#include <vector>

namespace khensu::copilot::reference {

/// A reference longer than this is cut (memory: 15 min of stereo float at
/// 48 kHz is ~350 MB).
inline constexpr double kMaxReferenceSeconds = 15.0 * 60.0;

/// The extensions the editor accepts for a reference (file chooser and drop).
inline const char* const kReferenceWildcard = "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg;*.m4a";
bool isReferenceFile(const juce::File& file);

struct ReferenceData {
    juce::String path;
    double sampleRate = 48000.0;
    juce::AudioBuffer<float> audio; ///< 1 or 2 channels at sampleRate
    LoudnessStats::Result stats;
    /// Short-term loudness at every 100 ms block (index = sample / blockSamples).
    std::vector<float> shortTermLufs;
    int blockSamples = 4800;
    mixhub::BandLevels bands = mixhub::silentBands(); ///< long-term mean on the hub grid
    bool truncated = false;                           ///< cut to kMaxReferenceSeconds

    double durationSec() const noexcept { return audio.getNumSamples() / juce::jmax(1.0, sampleRate); }
    juce::String name() const { return juce::File(path).getFileName(); }
    /// Short-term loudness at sample `position` (-96 when unknown).
    float shortTermAt(juce::int64 position) const noexcept;
};

using ReferencePtr = std::shared_ptr<const ReferenceData>;
using CancelCheck = std::function<bool()>;

/// THREAD_CONTEXT(WorkerThread). Decodes, resamples to `sampleRate` and
/// measures `file`. nullptr with `error` set when it cannot.
ReferencePtr loadReference(const juce::File& file, double sampleRate, juce::String& error,
                           const CancelCheck& cancelled = {});

/// THREAD_CONTEXT(AnyThread except audio). Measures audio already at
/// `sampleRate` (the tail of loadReference; tests use it directly).
ReferencePtr analyseReference(juce::AudioBuffer<float> audio, double sampleRate, const juce::String& path);

/// Long-term mean band levels of `audio` (mono downmix) on the Mix hub grid,
/// with the hub's own FFT size and normalisation, so they compare with a
/// track's hub frame.
mixhub::BandLevels averageBands(const juce::AudioBuffer<float>& audio, double sampleRate);

/// Band levels relative to their own power sum (dB), so two spectra of
/// different loudness compare by shape only.
mixhub::BandLevels normalisedShape(const mixhub::BandLevels& bands);

class ReferencePlayer {
  public:
    enum class MatchMode { Integrated = 0, ShortTerm = 1 };
    static constexpr float kMaxOffsetDb = 24.0f;
    static constexpr double kFadeSeconds = 0.015;
    static constexpr double kGainSmoothingSeconds = 0.3;

    /// What the audio thread knows about the host side this block.
    struct HostSide {
        bool advance = true;    ///< the host is playing (or reports no transport)
        bool timeValid = false; ///< timeSec is the host timeline position
        double timeSec = 0.0;
        float integratedLufs = -96.0f;
        float shortTermLufs = -96.0f;
        bool offline = false; ///< an offline render: never the reference
    };

    ReferencePlayer() = default;
    ~ReferencePlayer();
    ReferencePlayer(const ReferencePlayer&) = delete;
    ReferencePlayer& operator=(const ReferencePlayer&) = delete;

    /// Not concurrent with process().
    void prepare(double sampleRate) noexcept;

    /// Message thread: the reference the audio thread plays next (nullptr
    /// clears it). The audio thread adopts it at its next block.
    void setReference(ReferencePtr data);
    /// Message thread: frees what the audio thread handed back.
    void reapRetired();

    /// Any thread. Listening crossfades from the host to the reference.
    void setListening(bool listening) noexcept { listening_.store(listening, std::memory_order_relaxed); }
    bool isListening() const noexcept { return listening_.load(std::memory_order_relaxed); }
    void setMatchMode(MatchMode mode) noexcept { matchMode_.store(static_cast<int>(mode), std::memory_order_relaxed); }
    MatchMode matchMode() const noexcept { return static_cast<MatchMode>(matchMode_.load(std::memory_order_relaxed)); }

    /// AUDIO THREAD. Replaces (crossfades) `buffer` with the matched reference
    /// while listening. No allocation, no locks.
    void process(juce::AudioBuffer<float>& buffer, const HostSide& host) noexcept;

    /// The gain the reference plays at, in dB (the level offset readout).
    float offsetDb() const noexcept { return offsetDb_.load(std::memory_order_relaxed); }
    /// True once both sides had loudness to match on.
    bool isMatched() const noexcept { return matched_.load(std::memory_order_relaxed); }
    /// 0 = host only, 1 = reference only (mid-fade in between).
    float referenceMix() const noexcept { return mixOut_.load(std::memory_order_relaxed); }
    double positionSec() const noexcept { return positionSec_.load(std::memory_order_relaxed); }
    /// True when the audio thread holds a reference it can play at this rate.
    bool hasPlayableReference() const noexcept { return playable_.load(std::memory_order_relaxed); }

  private:
    struct Holder {
        ReferencePtr data;
    };
    void adoptPending() noexcept;
    float targetOffsetDb(const ReferenceData& ref, const HostSide& host, bool& ok) const noexcept;

    double sampleRate_ = 48000.0;
    int fadeSamples_ = 720;
    Holder* active_ = nullptr; ///< audio thread
    std::atomic<Holder*> pending_{nullptr};
    std::atomic<Holder*> retired_{nullptr};
    juce::SpinLock controlLock_; ///< control threads only (setReference / reap)

    std::atomic<bool> listening_{false};
    std::atomic<int> matchMode_{0};
    // Audio-thread state.
    juce::int64 position_ = 0;
    float mix_ = 0.0f;
    float gainDb_ = 0.0f;
    bool haveGain_ = false;
    // Published for the editor.
    std::atomic<float> offsetDb_{0.0f};
    std::atomic<bool> matched_{false};
    std::atomic<float> mixOut_{0.0f};
    std::atomic<double> positionSec_{0.0};
    std::atomic<bool> playable_{false};
};

} // namespace khensu::copilot::reference
