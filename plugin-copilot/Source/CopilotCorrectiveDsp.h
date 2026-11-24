#pragma once

#include <JuceHeader.h>
#include <khensu/dsp/LinkwitzRileyCrossover3Band.h>

#include <array>

namespace khensu {

/// Owns the built-in corrective chain (filters, EQ, compression, gain, and
/// chain-level dry/wet). APVTS remains owned by the processor; this class only
/// consumes the parameter atomics it is bound to.
///
/// Everything runs on the audio thread in fixed control sub-blocks of
/// kControlBlockSamples (PLG-01/PLG-08): parameter smoothing advances in AUDIO
/// time, and the HPF/LPF/peak coefficients are recomputed per sub-block with
/// IIR::ArrayCoefficients (no allocation) while a smoother is moving. There is
/// no coefficient worker thread, so an offline bounce is deterministic and a
/// parameter change can never be lost between two audio callbacks.
///
/// Stage on/off switches (and the single/multiband compressor switch)
/// crossfade over kStageFadeSeconds instead of cutting in hard (PLG-14).
///
/// OVH-8: each EQ band has a shape (bell / low shelf / high shelf), and a
/// split-band de-esser sits between the EQ and the compressor. Its high band
/// is the complement of a 2nd-order low-pass at deess_freq (high = x - LP(x)),
/// so the bands sum to the input exactly: the output LP + g * high is the
/// input, bit for bit, while the de-esser is idle (g = 1), turns everything
/// well above deess_freq down by the full reduction, and adds no latency. The
/// high band is also the (linked, peak) detector.
class CopilotCorrectiveDsp final {
  public:
    static constexpr int kControlBlockSamples = 32;
    static constexpr double kParamSmoothingSeconds = 0.02;
    static constexpr double kStageFadeSeconds = 0.010;

    CopilotCorrectiveDsp() = default;

    struct Parameters {
        std::atomic<float>* chainMix = nullptr;
        std::atomic<float>* hpfOn = nullptr;
        std::atomic<float>* hpfFreq = nullptr;
        std::atomic<float>* lpfOn = nullptr;
        std::atomic<float>* lpfFreq = nullptr;
        std::atomic<float>* eqOn = nullptr;
        std::atomic<float>* eqFreq = nullptr;
        std::atomic<float>* eqGainDb = nullptr;
        std::atomic<float>* eqQ = nullptr;
        std::atomic<float>* eq2On = nullptr;
        std::atomic<float>* eq2Freq = nullptr;
        std::atomic<float>* eq2GainDb = nullptr;
        std::atomic<float>* eq2Q = nullptr;
        std::atomic<float>* eq3On = nullptr;
        std::atomic<float>* eq3Freq = nullptr;
        std::atomic<float>* eq3GainDb = nullptr;
        std::atomic<float>* eq3Q = nullptr;
        std::atomic<float>* compOn = nullptr;
        std::atomic<float>* compThreshDb = nullptr;
        std::atomic<float>* compRatio = nullptr;
        std::atomic<float>* compAttackMs = nullptr;
        std::atomic<float>* compReleaseMs = nullptr;
        std::atomic<float>* compMultiband = nullptr;
        std::atomic<float>* compLowThreshDb = nullptr;
        std::atomic<float>* compMidThreshDb = nullptr;
        std::atomic<float>* compHighThreshDb = nullptr;
        std::atomic<float>* crossoverLowFreq = nullptr;
        std::atomic<float>* crossoverHighFreq = nullptr;
        std::atomic<float>* outGainDb = nullptr;
        // OVH-8 (state v3). Null = bell / de-esser off.
        std::atomic<float>* eqType = nullptr;
        std::atomic<float>* eq2Type = nullptr;
        std::atomic<float>* eq3Type = nullptr;
        std::atomic<float>* deessOn = nullptr;
        std::atomic<float>* deessFreq = nullptr;
        std::atomic<float>* deessThreshDb = nullptr;
        std::atomic<float>* deessRangeDb = nullptr;
    };

    /// De-esser ratio above the threshold (fixed 4:1) and envelope times.
    static constexpr float kDeessRatio = 4.0f;
    static constexpr double kDeessAttackSeconds = 0.0008;
    static constexpr double kDeessReleaseSeconds = 0.060;

    void bindParameters(const Parameters& parameters) { parameters_ = parameters; }
    void prepareToPlay(double sampleRate, int samplesPerBlock, int preparedChannels);
    void process(juce::AudioBuffer<float>& buffer, bool bypassed, int numSamples);
    /// Clears every filter/compressor/mixer state without touching
    /// coefficients or parameters. Audio-thread safe (no allocation); used to
    /// recover after a non-finite block poisoned the recursive states.
    void reset() noexcept;

    /// Test seam: the smoothed peak-1 gain (dB) the coefficients were last
    /// computed from. Audio thread only.
    float currentEqGainDbForTest() const noexcept { return smEqGainDb_.getCurrentValue(); }
    /// The de-esser's largest gain reduction (dB, >= 0) in the last
    /// process() call. Any thread.
    float lastDeessReductionDb() const noexcept { return deessReductionDb_.load(std::memory_order_relaxed); }
    /// VIZ-6b: the compressor stage's gain reduction (dB, >= 0) and its input
    /// level (RMS dBFS) in the last process() call; 0 / -120 while it is off.
    float lastCompReductionDb() const noexcept { return compReductionDb_.load(std::memory_order_relaxed); }
    float lastCompInputDb() const noexcept { return compInputDb_.load(std::memory_order_relaxed); }

  private:
    using StereoIIR =
        juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>, juce::dsp::IIR::Coefficients<float>>;
    using CoefficientValues = std::array<float, 6>;

    /// Linear 0..1 ramp used to crossfade a stage in/out. Audio thread only.
    struct StageFade {
        float value = 0.0f;
        float target = 0.0f;
        float step = 1.0f;
        void snap(bool on) noexcept { value = target = on ? 1.0f : 0.0f; }
        bool silent() const noexcept { return value <= 0.0f && target <= 0.0f; }
        bool settledOn() const noexcept { return value >= 1.0f && target >= 1.0f; }
        float next() noexcept
        {
            if (value < target)
                value = juce::jmin(target, value + step);
            else if (value > target)
                value = juce::jmax(target, value - step);
            return value;
        }
    };

    void processSubBlock(juce::dsp::AudioBlock<float> block);
    void advanceSmoothers(int numSamples);
    void updateCoefficients(bool force) noexcept;
    void readStageTargets() noexcept;
    template <typename ProcessFn>
    void runStage(StageFade& fade, juce::dsp::AudioBlock<float>& block, ProcessFn&& processFn);
    void processCompressor(juce::dsp::AudioBlock<float>& block, bool multiband);
    void processDeEsser(juce::dsp::AudioBlock<float>& block) noexcept;
    static int bandShape(const std::atomic<float>* type) noexcept;
    static void applyCoefficientValues(StereoIIR& filter, const CoefficientValues& values) noexcept;

    Parameters parameters_;
    StereoIIR hpf_, lpf_, peak_, peak2_, peak3_;
    StereoIIR deessSplit_;            ///< the de-esser's split low-pass (high = x - LP)
    std::array<int, 3> eqShape_{};    ///< shape each band's coefficients were built for
    juce::dsp::Compressor<float> comp_;
    LinkwitzRileyCrossover3Band crossover_;
    juce::dsp::Compressor<float> compLow_, compMid_, compHigh_;
    juce::dsp::Gain<float> outGain_;
    juce::dsp::DryWetMixer<float> dryWet_;
    double sampleRate_ = 44100.0;
    int preparedChannels_ = 2;
    // JUCE's process spec is a host contract. Keep the maximum block size so
    // process() can fail closed instead of resizing a buffer on the audio
    // thread when a host violates it.
    int preparedBlockSize_ = 1;
    juce::AudioBuffer<float> lowBandBuffer_, midBandBuffer_, highBandBuffer_;
    // Stage crossfade scratch (kControlBlockSamples per channel).
    juce::AudioBuffer<float> stageDry_, compAlt_, deessHigh_;
    std::array<float, kControlBlockSamples> fadeGains_{};

    juce::SmoothedValue<float> smHpfFreq_, smLpfFreq_, smEqFreq_, smEqGainDb_, smEqQ_;
    juce::SmoothedValue<float> smEq2Freq_, smEq2GainDb_, smEq2Q_;
    juce::SmoothedValue<float> smEq3Freq_, smEq3GainDb_, smEq3Q_;
    juce::SmoothedValue<float> smCompThresh_, smCompRatio_, smCompAttack_, smCompRelease_;
    juce::SmoothedValue<float> smCompLowThresh_, smCompMidThresh_, smCompHighThresh_;
    juce::SmoothedValue<float> smCrossoverLowFreq_, smCrossoverHighFreq_;
    juce::SmoothedValue<float> smChainMix_;
    juce::SmoothedValue<float> smDeessFreq_, smDeessThresh_, smDeessRange_;
    float deessEnv_ = 0.0f;
    float deessAttack_ = 1.0f, deessRelease_ = 1.0f; ///< one-pole coefficients per sample
    float deessBlockMaxGr_ = 0.0f;
    std::atomic<float> deessReductionDb_{0.0f};
    std::atomic<float> compReductionDb_{0.0f};
    std::atomic<float> compInputDb_{-120.0f};

    StageFade hpfFade_, lpfFade_, eqFade_, eq2Fade_, eq3Fade_, compFade_, multibandFade_, deessFade_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CopilotCorrectiveDsp)
};

} // namespace khensu
