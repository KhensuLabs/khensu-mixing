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
    };

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
    static void applyCoefficientValues(StereoIIR& filter, const CoefficientValues& values) noexcept;

    Parameters parameters_;
    StereoIIR hpf_, lpf_, peak_, peak2_, peak3_;
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
    juce::AudioBuffer<float> stageDry_, compAlt_;
    std::array<float, kControlBlockSamples> fadeGains_{};

    juce::SmoothedValue<float> smHpfFreq_, smLpfFreq_, smEqFreq_, smEqGainDb_, smEqQ_;
    juce::SmoothedValue<float> smEq2Freq_, smEq2GainDb_, smEq2Q_;
    juce::SmoothedValue<float> smEq3Freq_, smEq3GainDb_, smEq3Q_;
    juce::SmoothedValue<float> smCompThresh_, smCompRatio_, smCompAttack_, smCompRelease_;
    juce::SmoothedValue<float> smCompLowThresh_, smCompMidThresh_, smCompHighThresh_;
    juce::SmoothedValue<float> smCrossoverLowFreq_, smCrossoverHighFreq_;
    juce::SmoothedValue<float> smChainMix_;

    StageFade hpfFade_, lpfFade_, eqFade_, eq2Fade_, eq3Fade_, compFade_, multibandFade_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CopilotCorrectiveDsp)
};

} // namespace khensu
