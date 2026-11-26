#pragma once
// OVH-9: the native half of the service's chain renderer
// (copilot/dsp.py _run_serial_chain). A chain of EffectSpec-shaped stages runs
// block by block over planar double audio, so a long file never has to be in
// memory at once. Each stage reproduces the service's processor for that
// effect - same parameter clamps and defaults (dsp_params._clamped_params),
// same filter designs, same dynamics laws:
//
//   HighpassFilter / LowpassFilter  causal Butterworth, order = slope / 6
//                                   (dsp_filters.butterworth_filter)
//   PeakFilter / LowShelfFilter /   pedalboard's (JUCE) RBJ biquads
//   HighShelfFilter
//   Compressor                      pedalboard's (juce::dsp::Compressor):
//                                   peak ballistics, per channel, no knee
//   DeEsser                         dsp_dynamics._apply_deesser: zero-phase
//                                   4th-order band split, 8 ms envelope
//   Gain                            a plain multiply
//   Limiter                         tp_limiter.true_peak_limit: 8x true-peak
//                                   ceiling, 1.5 ms lookahead, no makeup
//   SpectralDenoise (FN2-2a)        dsp_dynamics._apply_spectral_denoise: a
//                                   whole-signal noise profile (a profile
//                                   pass, below), then STFT Wiener gains
//                                   (SpectralDenoiser.h)
//   TransientShaper (FN2-2a)        dsp_dynamics._apply_transient_shaper:
//                                   linked fast/slow envelope blend
//   TapeSaturation (FN2-2a)         dsp_color._apply_tape_saturation: 4x
//                                   resample_poly, tanh blend, DC blocker
//                                   (TapeSaturator.h)
//   StereoWidener (FN2-2a)          dsp_spatial._apply_stereo_widener: M/S
//   DynamicEQ, MultibandCompressor, MidSideProcessor, Exciter, BassMonoMaker,
//   NoiseGate (ENH-6)               see ExtendedChainStages.h; DynamicEQ can
//                                   key on a second signal (attachKey)
//
// So every effect buildRecommendationChain emits renders natively. Everything
// else (reverbs, ...) needs the engine: prepare() refuses the chain and names
// them. Stages with lookahead (DeEsser, Limiter, SpectralDenoise,
// TapeSaturation) hold samples back and release them in finish(), so the
// output is exactly as long as the input and sample-aligned with it.
//
// A stage whose processing depends on the whole signal (SpectralDenoise's
// noise profile is a per-bin percentile over the track) says so with
// needsProfile(): before the render pass the caller replays the input once
// per such stage (runProfilePasses) - through fresh copies of the stages in
// front of it - so memory stays bounded on any length.
// THREAD_CONTEXT(WorkerThread) - one instance per render, not shared.
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace khensu::dsp {

/// One effect of a chain, as the service's apply-chain request names it.
struct ChainStageSpec {
    std::string effect;
    std::map<std::string, float> parameters;
};

/// Planar double audio: ch[channel][sample], every channel the same length.
struct PlanarBlock {
    std::vector<std::vector<double>> ch;
    int numSamples() const noexcept { return ch.empty() ? 0 : static_cast<int>(ch.front().size()); }
    void clear() noexcept
    {
        for (auto& c : ch) c.clear();
    }
};

/// True when the native renderer implements `effect`.
bool isNativeChainEffect(const std::string& effect);
/// The effects of `chain` only the engine can render (empty: all native).
std::vector<std::string> effectsNeedingEngine(const std::vector<ChainStageSpec>& chain);
/// dsp.chain_output_ceiling_dbtp: the last Limiter's threshold_db (or
/// Clipper's ceiling_db), clamped as the stage clamps it, else -1 dBTP.
double chainOutputCeilingDbtp(const std::vector<ChainStageSpec>& chain);
/// The Limiter's own target: its ceiling less the 0.05 dB safety margin
/// (tp_limiter._MARGIN_DB) - what its residual trim aims for.
double limiterTargetDbtp(const ChainStageSpec& limiter);
/// STU-18: every parameter of a native stage as the stage reads it (given
/// values clamped, frequencies held below 0.45 x `sampleRate`, the rest the
/// defaults) - for a live stand-in (RealtimeChain) that must read the same
/// numbers. nullopt + `error` where makeChainStage would fail.
std::optional<std::map<std::string, double>> resolveChainStageParams(const ChainStageSpec& spec, double sampleRate,
                                                                     std::string& error);

/// One processor of the chain.
class ChainStage {
  public:
    virtual ~ChainStage() = default;
    /// Replaces `io` with whatever output is ready (a stage with lookahead
    /// returns fewer samples at first and catches up in finish()).
    virtual void push(PlanarBlock& io) = 0;
    /// After the last push: `tail` (empty on entry) receives what is left.
    virtual void finish(PlanarBlock& tail) { tail.clear(); }

    /// STU-9: true when push() processes the block in place - same length
    /// out as in, no lookahead, no whole-signal profile and no allocation -
    /// so the stage can run block by block on the audio thread
    /// (RealtimeChain) and match the offline render sample for sample.
    virtual bool isCausalInPlace() const { return false; }

    /// True when the stage must see the whole signal before processing it
    /// (profile() with every block, then endProfile(), then push()).
    virtual bool needsProfile() const { return false; }
    virtual void profile(const PlanarBlock& /*in*/) {}
    virtual void endProfile() {}
    /// Takes the finished profile of `other` (the same effect, parameters
    /// and format) - for the fresh copies a later stage's profile pass runs.
    virtual void adoptProfile(const ChainStage& /*other*/) {}

    /// ENH-6: a stage that can detect on a KEY signal (a keyed DynamicEQ).
    /// Without attachKey() it keys on its own input.
    virtual bool wantsKey() const { return false; }
    /// Before the first push: the key arrives with `numKeyChannels` channels.
    virtual void attachKey(int /*numKeyChannels*/) {}
    /// The key at the file positions of the input block the chain was just
    /// given (it reaches the stage before that input does).
    virtual void pushKey(const PlanarBlock& /*key*/) {}
    /// No more key (before the chain's finish()).
    virtual void endKey() {}
};

/// Builds the stage for `spec` at `sampleRate`; nullptr with `error` set when
/// the effect is not native or a parameter is unusable (non-finite, or a
/// frequency this sample rate cannot run - the service refuses those too).
std::unique_ptr<ChainStage> makeChainStage(const ChainStageSpec& spec, double sampleRate, int numChannels,
                                           std::string& error);

/// A whole chain, stage after stage.
class ChainProcessor {
  public:
    using Sink = std::function<void(const PlanarBlock&)>;
    /// False with `error` when a stage cannot be built (see makeChainStage).
    bool prepare(const std::vector<ChainStageSpec>& chain, double sampleRate, int numChannels, std::string& error);
    /// Runs `block` through the chain and hands the output to `sink` (it may
    /// be empty or shorter than the input while a lookahead fills).
    /// ENH-6: `key` (when attachKey() was called) is the key signal at the
    /// same file positions as `block` - the same length.
    void process(PlanarBlock block, const Sink& sink, const PlanarBlock* key = nullptr);
    /// ENH-6: routes a key signal with `numKeyChannels` channels to every
    /// stage that keys on one (after prepare(), before the first process()).
    /// False when no stage uses a key. A profile pass's copies of the stages
    /// in front of a profiled stage stay self-keyed.
    bool attachKey(int numKeyChannels);
    /// Whether a stage of the chain asks for a key (DynamicEQ use_key = 1).
    bool wantsKey() const;
    /// Flushes every stage's held-back samples through the rest of the chain.
    void finish(const Sink& sink);
    int numChannels() const noexcept { return numChannels_; }

    /// Whether a stage needs a profile pass before process() (see above).
    bool needsProfilePass() const;
    /// Feeds the chain's whole input, block by block, to `consume`; false
    /// when it stopped (cancelled, decode error).
    using Replay = std::function<bool(const std::function<void(PlanarBlock&&)>& consume)>;
    /// One replay per stage that needs a profile, in chain order: the input
    /// runs through fresh copies of the stages in front of it into its
    /// profile(). False with `error` when a replay fails. Call once, after
    /// prepare() and before the first process().
    bool runProfilePasses(const Replay& replay, std::string& error);

  private:
    std::vector<std::unique_ptr<ChainStage>> stages_;
    std::vector<ChainStageSpec> specs_;
    double sampleRate_ = 0.0;
    int numChannels_ = 0;
    bool keyAttached_ = false;
};

} // namespace khensu::dsp
