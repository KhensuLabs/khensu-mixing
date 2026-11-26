#pragma once

#include <JuceHeader.h>
#include <khensu/dsp/LinkwitzRileyCrossover3Band.h>
#include <khensu/dsp/RealtimeStages.h>

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
///
/// STU-10 (state v5): six EQ bands (khensu::dsp::RealtimeEqBand), each a
/// bell / shelf / notch / high- / low-pass on stereo, mid or side, with an
/// optional self-keyed dynamic mode; then, after the compressor, a transient
/// shaper, tape saturation, bass mono and stereo width - all zero latency,
/// each faded in and out like the other stages. A band that is stereo,
/// static and of its v4 shape runs the same filter as before, so a v4 state
/// sounds the same.
class CopilotCorrectiveDsp final {
  public:
    static constexpr int kControlBlockSamples = 32;
    static_assert(kControlBlockSamples == dsp::RealtimeEqBand::kControlSamples);
    static constexpr int kNumBands = 6;
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

        // STU-10 (state v5). Null = the plugin defaults: bands 4-6 off, a
        // band's kind "Shape", stereo and static; the new stages off.
        struct Band {
            /// Bands 4-6 only: bands 1-3 read eqOn / eqFreq / ... above
            /// (a non-null value here overrides them).
            std::atomic<float>* on = nullptr;
            std::atomic<float>* freq = nullptr;
            std::atomic<float>* gainDb = nullptr;
            std::atomic<float>* q = nullptr;
            std::atomic<float>* type = nullptr;
            // Every band.
            std::atomic<float>* kind = nullptr; ///< mixerchain::EqBandKind
            std::atomic<float>* mode = nullptr; ///< mixerchain::EqBandChannels
            std::atomic<float>* dynOn = nullptr;
            std::atomic<float>* dynThreshDb = nullptr;
            std::atomic<float>* dynRangeDb = nullptr;
            std::atomic<float>* dynAttackMs = nullptr;
            std::atomic<float>* dynReleaseMs = nullptr;
        };
        std::array<Band, kNumBands> bands{};
        std::atomic<float>* tsOn = nullptr;
        std::atomic<float>* tsAttackDb = nullptr;
        std::atomic<float>* tsSustainDb = nullptr;
        std::atomic<float>* tsAttackMs = nullptr;
        std::atomic<float>* tsReleaseMs = nullptr;
        std::atomic<float>* satOn = nullptr;
        std::atomic<float>* satDriveDb = nullptr;
        std::atomic<float>* satWarmth = nullptr;
        std::atomic<float>* satMix = nullptr;
        std::atomic<float>* satTrimDb = nullptr;
        std::atomic<float>* bassMonoOn = nullptr;
        std::atomic<float>* bassMonoFreq = nullptr;
        std::atomic<float>* widthOn = nullptr;
        std::atomic<float>* widthAmount = nullptr;
    };

    /// De-esser ratio above the threshold (fixed 4:1) and envelope times.
    static constexpr float kDeessRatio = 4.0f;
    static constexpr double kDeessAttackSeconds = 0.0008;
    static constexpr double kDeessReleaseSeconds = 0.060;

    void bindParameters(const Parameters& parameters);
    void prepareToPlay(double sampleRate, int samplesPerBlock, int preparedChannels);
    void process(juce::AudioBuffer<float>& buffer, bool bypassed, int numSamples);
    /// Clears every filter/compressor/mixer state without touching
    /// coefficients or parameters. Audio-thread safe (no allocation); used to
    /// recover after a non-finite block poisoned the recursive states.
    void reset() noexcept;

    /// Test seam: the smoothed peak-1 gain (dB) the coefficients were last
    /// computed from. Audio thread only.
    float currentEqGainDbForTest() const noexcept { return smBandGain_[0].getCurrentValue(); }
    /// Test seam: band `index` (0-based)'s current dynamic reduction (dB).
    float bandReductionDbForTest(int index) const noexcept
    {
        return bands_[static_cast<size_t>(juce::jlimit(0, kNumBands - 1, index))].currentReductionDb();
    }
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
    /// The band's filter type: its kind, else its v3 shape.
    int bandType(int index) const noexcept;
    int bandMode(int index) const noexcept;
    dsp::RealtimeEqBand::Dynamics bandDynamics(int index) const noexcept;
    static void applyCoefficientValues(StereoIIR& filter, const CoefficientValues& values) noexcept;

    /// A band's parameters, resolved at bind time (bands 1-3 from the v1/v3
    /// fields). Null = the default.
    struct BoundBand {
        const std::atomic<float>*on = nullptr, *freq = nullptr, *gainDb = nullptr, *q = nullptr, *type = nullptr;
        Parameters::Band ext;
    };

    Parameters parameters_;
    std::array<BoundBand, kNumBands> bound_{};
    StereoIIR hpf_, lpf_;
    std::array<dsp::RealtimeEqBand, kNumBands> bands_;
    StereoIIR deessSplit_;            ///< the de-esser's split low-pass (high = x - LP)
    std::array<int, kNumBands> bandType_{}; ///< type each band's coefficients were built for
    std::array<int, kNumBands> bandMode_{}; ///< and its channels
    dsp::RealtimeTransientShaper transient_;
    dsp::RealtimeSaturation saturation_;
    dsp::RealtimeBassMono bassMono_;
    dsp::RealtimeStereoWidth width_;
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

    juce::SmoothedValue<float> smHpfFreq_, smLpfFreq_;
    std::array<juce::SmoothedValue<float>, kNumBands> smBandFreq_, smBandGain_, smBandQ_;
    juce::SmoothedValue<float> smTsAttack_, smTsSustain_, smSatDrive_, smSatWarmth_, smSatMix_, smSatTrim_;
    juce::SmoothedValue<float> smBassMonoFreq_, smWidth_;
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

    StageFade hpfFade_, lpfFade_, compFade_, multibandFade_, deessFade_;
    std::array<StageFade, kNumBands> bandFades_{};
    StageFade tsFade_, satFade_, bassMonoFade_, widthFade_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CopilotCorrectiveDsp)
};

} // namespace khensu
