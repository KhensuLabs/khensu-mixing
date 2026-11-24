#pragma once

#include <JuceHeader.h>
#include <khensu/dsp/LookaheadBrickwallLimiter.h>
#include "GrowableDelayLine.h"
#include <atomic>
#include <cmath>
#include <memory>
#include <vector>

namespace khensu {

/// Owns the post-chain output contract: true-peak lookahead, latency-aware
/// bypass, and the level-matched A/B comparison.
///
/// A/B modes: 0 = Processed (B, untouched), 1 = Original (A), 2 = Delta,
/// 3 = Blind (A or B at random, hidden). The ORIGINAL side is level-matched
/// to the processed side using running RMS, so switching A/B compares tone
/// and dynamics rather than loudness, while normal processed output is never
/// altered. Delta subtracts the level-matched original, leaving only what
/// the processing changed.
///
/// The limiter ceiling holds in every A/B mode (FIX-12). The A/B switch sits
/// after the limiter, so the Original (dry x match gain, up to +12 dB) and
/// Delta used to reach the output with nothing to stop them. While the
/// limiter is on:
///  - Original: the match gain is capped by the headroom of the dry side, so
///    its recent sample peak lands on the ceiling at most. It stays the dry
///    signal times ONE gain for both channels - not limited, not clipped. When
///    the cap engages the Original is quieter than a full level match by
///    monitorTrimDb().
///  - Delta: computed with the uncapped match gain (so it nulls as deeply as
///    before), then turned down as a whole when its own peak is over the
///    ceiling.
///  - Blind: the hidden processed side takes the same trim as the hidden
///    original, so the trial stays level matched.
///  - Processed (mode 0) and bypass are never touched.
/// With the limiter off there is no ceiling in any mode, as before. The guard
/// works on sample peaks: the Original and Delta sides have no oversampled
/// detector, so their inter-sample peaks can pass the ceiling.
///
/// True-peak alignment (PLG-02 / CORE-02): the 4x oversampled detector sees
/// each sample `upsampleDelay` base-rate samples late (the FIR group delay,
/// measured at prepare). The audio is therefore delayed by
/// alignSamples() = ceil(upsampleDelay) before the brickwall limiter, so the
/// detector value handed to the limiter for sample n describes that same
/// sample. Total reported latency = alignSamples() + lookahead. The
/// oversampler runs on every block (also with the limiter off) so its state is
/// never stale when the limiter is switched on.
///
/// A/B switches and global bypass crossfade over kSwitchFadeSeconds instead
/// of hard-cutting (AUDIT-086). While a bypass fade is running the processed
/// path must still be computed, which the processor asks via
/// needsProcessedPath() before running the chain.
class CopilotOutputStage final {
  public:
    /// VIZ-6b: the limiter's deepest gain reduction in the last block (dB, >= 0).
    float lastLimiterReductionDb() const noexcept { return limiterReductionDb_.load(std::memory_order_relaxed); }
    static constexpr double kSwitchFadeSeconds = 0.015;

    CopilotOutputStage() = default;

    void prepareToPlay(double sampleRate, int samplesPerBlock, int preparedChannels);
    void captureDry(const juce::AudioBuffer<float>& buffer, int abMode = 0);
    /// `reportedHostedLatency` is what the processor reports for the hosted
    /// chain; when globally bypassed the chain is skipped, so the dry signal
    /// is delayed by the full reported latency to keep the host aligned.
    void process(juce::AudioBuffer<float>& buffer, bool globalBypass, bool limiterOn, float limiterThresholdDb,
                 int abMode, int activeHostedLatency, int reportedHostedLatency = 0);

    /// Control thread (FIX-14): makes the A/B dry line long enough for a
    /// hosted chain of `hostedLatencySamples`, so Original stays aligned
    /// behind more than a second of hosted latency. Allocates off the audio
    /// thread; the audio thread adopts the bigger line at its next block.
    void reserveHostedLatency(int hostedLatencySamples)
    {
        abDryDelay_.reserve(latencySamples() + juce::jmax(0, hostedLatencySamples) + 8);
    }
    /// Control thread: frees a dry line the audio thread has replaced.
    void reapRetired() { abDryDelay_.reapRetired(); }
    /// Delay the A/B dry line can apply right now (tests).
    int abDryCapacityForTesting() const noexcept { return abDryDelay_.activeCapacity(); }

    /// False only once a bypass fade has fully settled: the processor may then
    /// skip the corrective and hosted chains. During a fade (either way) the
    /// processed signal is still needed to crossfade from/to.
    bool needsProcessedPath(bool globalBypass) const noexcept { return !globalBypass || bypassMix_ < 1.0f; }

    /// Jumps the bypass crossfade to its end state (used after prepare, so a
    /// project loaded bypassed doesn't fade out on its first blocks).
    void snapBypass(bool bypassed) noexcept
    {
        bypassMix_ = bypassed ? 1.0f : 0.0f;
        bypassHold_ = 0;
    }

    /// Clears every piece of signal history (limiter, dry delay, oversampler,
    /// level-match EMAs). Audio-thread safe; used to recover from a
    /// non-finite block (AUDIT-025).
    void resetState() noexcept;

    /// Limiter lookahead only (the gain ramp length).
    int lookaheadSamples() const noexcept { return lookaheadSamples_; }
    /// Audio delay that aligns the oversampled detector with the audio.
    int alignSamples() const noexcept { return alignSamples_; }
    /// Everything this stage delays the audio by: what the processor reports
    /// to the host on top of the hosted-chain latency.
    int latencySamples() const noexcept { return lookaheadSamples_ + alignSamples_; }
    /// Measured upsampling group delay of the detector, in base-rate samples.
    float detectorDelaySamples() const noexcept { return detectorDelay_; }

    /// Gain that level-matches the original (A) side to B, in dB. This is the
    /// match TARGET; what is heard is this plus monitorTrimDb().
    float matchGainDb() const noexcept { return matchGainDb_.load(std::memory_order_relaxed); }
    /// How far the side being heard is turned down to keep it under the
    /// limiter ceiling, in dB (<= 0; FIX-12). 0 in Processed mode, in bypass,
    /// with the limiter off, and whenever the matched side fits. Non-zero in
    /// Original mode means the comparison is NOT level matched right now.
    float monitorTrimDb() const noexcept { return monitorTrimDb_.load(std::memory_order_relaxed); }
    /// For Blind mode: true when the hidden trial is playing the processed side.
    bool blindIsProcessed() const noexcept { return blindIsProcessed_.load(std::memory_order_relaxed); }
    /// Blind mode: draw a new hidden side on the next audio block (any thread).
    void requestBlindReshuffle() noexcept { blindReshuffle_.store(true, std::memory_order_release); }

  private:
    /// The audible result of an A/B mode once Blind has been resolved.
    /// BlindProcessed is the processed side of a Blind trial: the processed
    /// signal with the same ceiling trim as the Original, so both hidden
    /// sides match.
    enum class Listen { Processed, Original, Delta, BlindProcessed };

    /// Largest sample magnitude seen recently: instant attack, a hold, then a
    /// slow release. Audio thread only.
    struct PeakHold {
        double level = 0.0;
        int hold = 0;

        void reset() noexcept
        {
            level = 0.0;
            hold = 0;
        }

        void push(float peak, int holdSamples, double release) noexcept
        {
            const double p = peak;
            if (std::isfinite(p) && p >= level) {
                level = p;
                hold = holdSamples;
            } else if (hold > 0) {
                --hold;
            } else {
                level *= release;
                if (level < 1.0e-9) level = 0.0;
            }
        }
    };

    float measureUpsampleDelay(int blockSize);

    int preparedChannels_ = 2;
    int lookaheadSamples_ = 0;
    int alignSamples_ = 0;
    float detectorDelay_ = 0.0f;
    double sampleRate_ = 44100.0;
    std::unique_ptr<juce::dsp::Oversampling<float>> oversampler_;
    std::vector<float> upsampledPeak_;
    std::vector<float> detectorPeak_;
    float previousUpsampledPeak_ = 0.0f;
    juce::AudioBuffer<float> alignRing_;
    int alignPos_ = 0;
    juce::AudioBuffer<float> abDryBuffer_;
    copilot::GrowableDelayLine abDryDelay_;
    khensu::dsp::LookaheadBrickwallLimiter brickwall_;

    // Level matching: exponential running mean-squares (3 s time constant)
    // of the processed and delayed-dry signals, and a smoothed gain.
    double wetMs_ = 0.0, dryMs_ = 0.0;
    float matchGain_ = 1.0f;
    std::atomic<float> matchGainDb_{0.0f};
    std::atomic<float> limiterReductionDb_{0.0f}; ///< VIZ-6b, the last block's deepest reduction
    // Ceiling guard for the A/B sides (FIX-12): recent sample peaks of the
    // aligned dry and of the delta, tracked in every mode so they are warm
    // when the listener switches.
    PeakHold dryPeak_, deltaPeak_;
    int peakHoldSamples_ = 1;
    double peakRelease_ = 1.0;
    std::atomic<float> monitorTrimDb_{0.0f};
    int lastAbMode_ = 0;
    juce::Random blindRandom_;
    std::atomic<bool> blindIsProcessed_{false};
    std::atomic<bool> blindReshuffle_{false};

    // Switch crossfades (audio thread only).
    int fadeSamples_ = 1;
    float bypassMix_ = 0.0f; ///< 0 = processed output, 1 = bypassed (aligned dry)
    bool wasBypassSettled_ = false;
    int bypassHold_ = 0; ///< samples to stay on the dry after leaving bypass
    Listen listenFrom_ = Listen::Processed;
    Listen listenTo_ = Listen::Processed;
    int listenFadePos_ = 0; ///< samples into the A/B crossfade; >= fadeSamples_ when settled

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CopilotOutputStage)
};

} // namespace khensu
