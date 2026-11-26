#pragma once
// STU-9: the native chain renderer's stages (ChainProcessor.h) run live, block
// by block, on the audio thread - so a chain can be heard while it is edited,
// before Keep renders it offline.
//
// Which stages run, and how (plan()):
//   Exact             a stage whose push() is causal and in place
//                     (ChainStage::isCausalInPlace: the filters and shelves,
//                     Compressor, Gain, TransientShaper, StereoWidener,
//                     MidSideProcessor, NoiseGate) - the same code as the
//                     offline render, so the preview matches it sample for
//                     sample (up to float conversion).
//   CausalEquivalent  a causal stand-in, reported as "substituted" with a
//                     note - close to the render, not exact:
//                     Limiter: the offline stage holds samples back in
//                     growing queues; the preview runs the plugin's
//                     LookaheadBrickwallLimiter (sample-peak, 1.5 ms
//                     lookahead) at the same ceiling, which adds 1.5 ms of
//                     latency.
//                     STU-18 (RealtimeStages.h, zero latency):
//                     BassMonoMaker -> RealtimeBassMono at the same
//                       crossover (an LR4 split instead of zero-phase bands);
//                     TapeSaturation -> RealtimeSaturation, the same curve,
//                       drive, warmth, mix and trim without 4x oversampling;
//                     DynamicEQ -> a dynamic RealtimeEqBand (bell 1/q octaves
//                       wide, or the shelf), same threshold / ratio / max
//                       reduction / attack / release, always self-keyed;
//                     DeEsser -> a dynamic RealtimeEqBand bell over the
//                       de-esser's band (f/1.6 .. 1.6 f), 8 ms ballistics;
//                     Exciter -> the same high band, tanh(drive (b + even
//                       b^2)) high-passed again and scaled to the band's
//                       running RMS (the render scales to the whole track's).
//   Skipped           stages that need the whole signal: MultibandCompressor,
//                     SpectralDenoise - and any effect only the analysis
//                     engine renders. They are heard in the render only; the
//                     caller shows plan()'s notes.
//
// Loudness match (on by default): the preview is not allowed to win by
// being louder. The output is scaled by sqrt(input power / output power),
// both averaged over ~0.8 s (frozen in silence, clamped to +-18 dB), and a
// new chain starts from the gain its predecessor reached, adjusted by the
// difference in the chains' Gain stages (adoptMatchState) - so a knob move
// does not jump in level while the averages settle.
//
// THREAD_CONTEXT: prepare() / plan() off the audio thread (it allocates);
// process() / reset() on the audio thread - no allocation, no locks, as long
// as numChannels <= the prepared count (extra channels pass unchanged).
#include <khensu/dsp/ChainProcessor.h>

#include <memory>
#include <string>
#include <vector>

namespace khensu::dsp {

class RealtimeChain {
  public:
    enum class StageMode { Exact, CausalEquivalent, Skipped };
    struct StagePlan {
        std::string effect;
        StageMode mode = StageMode::Skipped;
        std::string note; ///< why a stage is skipped or substituted ("" when Exact)
    };

    /// Builds the stages for `chain` at the playback device's rate. Never
    /// fails on a stage it cannot run live - that stage is Skipped with a
    /// note; false (with `error`) only for an unusable format.
    bool prepare(const std::vector<ChainStageSpec>& chain, double sampleRate, int numChannels, int maxBlockSize,
                 std::string& error);

    const std::vector<StagePlan>& plan() const noexcept { return plan_; }
    /// Samples the preview delays the signal by (the limiter's lookahead).
    int latencySamples() const noexcept { return latency_; }
    /// True when no stage processes (an empty or all-skipped chain): the
    /// output is the input.
    bool isIdentity() const noexcept { return slots_.empty(); }
    double sampleRate() const noexcept { return sampleRate_; }
    /// The static gain the chain's Gain stages add (dB) - the prediction the
    /// loudness match starts from.
    double predictedGainDb() const noexcept { return predictedGainDb_; }

    /// Before the chain is handed to the audio thread.
    void setLoudnessMatch(bool on) noexcept { match_ = on; }
    bool loudnessMatch() const noexcept { return match_; }

    /// AUDIO THREAD. Processes `numSamples` of `channels[0..numChannels)`
    /// in place.
    void process(float* const* channels, int numChannels, int numSamples) noexcept;
    /// AUDIO THREAD. Restarts the loudness match from the prediction (the
    /// stages keep their state: a fresh chain is a new prepare()).
    void resetMatch() noexcept;
    /// AUDIO THREAD. Continue `previous`'s loudness match (see above).
    void adoptMatchState(const RealtimeChain& previous) noexcept;
    /// The gain the match applies now (dB).
    float matchGainDb() const noexcept;

    /// A CausalEquivalent stand-in: float, in place, at most the prepared
    /// channel count; allocation-free and lock-free in process().
    class LiveStage {
      public:
        virtual ~LiveStage() = default;
        virtual void process(float* const* channels, int numChannels, int numSamples) noexcept = 0;
        virtual int latencySamples() const noexcept { return 0; }
    };

  private:
    struct Slot {
        std::unique_ptr<ChainStage> stage; ///< Exact
        std::unique_ptr<LiveStage> live;   ///< CausalEquivalent
    };
    void processChunk(float* const* channels, int numChannels, int offset, int n) noexcept;

    std::vector<StagePlan> plan_;
    std::vector<Slot> slots_;
    PlanarBlock scratch_;                     ///< capacity maxBlock_ per channel
    std::vector<std::vector<float>> liveBuf_; ///< float copy for the stand-ins
    std::vector<float*> livePtrs_;
    double sampleRate_ = 0.0;
    int numChannels_ = 0, maxBlock_ = 0, latency_ = 0;
    double predictedGainDb_ = 0.0;

    bool match_ = true;
    double inPower_ = 0.0, outPower_ = 0.0; ///< averaged mean squares
    bool matchPrimed_ = false;
    float gain_ = 1.0f; ///< the match gain reached
};

} // namespace khensu::dsp
