#include "CopilotCorrectiveDsp.h"

namespace khensu {

namespace {
float loadParam(const std::atomic<float>* value) noexcept
{
    return value != nullptr ? value->load(std::memory_order_relaxed) : 0.0f;
}

bool loadSwitch(const std::atomic<float>* value) noexcept { return loadParam(value) > 0.5f; }
} // namespace

void CopilotCorrectiveDsp::prepareToPlay(double sampleRate, int samplesPerBlock, int preparedChannels)
{
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
    preparedChannels_ = juce::jlimit(1, 2, preparedChannels);
    const int blockSize = juce::jmax(1, samplesPerBlock);
    preparedBlockSize_ = blockSize;
    const juce::dsp::ProcessSpec spec{sampleRate_, static_cast<juce::uint32>(blockSize),
                                      static_cast<juce::uint32>(preparedChannels_)};

    hpf_.prepare(spec);
    lpf_.prepare(spec);
    peak_.prepare(spec);
    peak2_.prepare(spec);
    peak3_.prepare(spec);
    comp_.prepare(spec);
    crossover_.prepare(spec);
    compLow_.prepare(spec);
    compMid_.prepare(spec);
    compHigh_.prepare(spec);
    lowBandBuffer_.setSize(preparedChannels_, blockSize);
    midBandBuffer_.setSize(preparedChannels_, blockSize);
    highBandBuffer_.setSize(preparedChannels_, blockSize);
    stageDry_.setSize(preparedChannels_, kControlBlockSamples);
    compAlt_.setSize(preparedChannels_, kControlBlockSamples);
    outGain_.prepare(spec);
    dryWet_.prepare(spec);
    dryWet_.setMixingRule(juce::dsp::DryWetMixingRule::linear);
    outGain_.setRampDurationSeconds(kParamSmoothingSeconds);

    auto initSmooth = [this](juce::SmoothedValue<float>& value, const std::atomic<float>* source) {
        value.reset(sampleRate_, kParamSmoothingSeconds);
        value.setCurrentAndTargetValue(loadParam(source));
    };
    initSmooth(smHpfFreq_, parameters_.hpfFreq);
    initSmooth(smLpfFreq_, parameters_.lpfFreq);
    initSmooth(smEqFreq_, parameters_.eqFreq);
    initSmooth(smEqGainDb_, parameters_.eqGainDb);
    initSmooth(smEqQ_, parameters_.eqQ);
    initSmooth(smEq2Freq_, parameters_.eq2Freq);
    initSmooth(smEq2GainDb_, parameters_.eq2GainDb);
    initSmooth(smEq2Q_, parameters_.eq2Q);
    initSmooth(smEq3Freq_, parameters_.eq3Freq);
    initSmooth(smEq3GainDb_, parameters_.eq3GainDb);
    initSmooth(smEq3Q_, parameters_.eq3Q);
    initSmooth(smCompThresh_, parameters_.compThreshDb);
    initSmooth(smCompRatio_, parameters_.compRatio);
    initSmooth(smCompAttack_, parameters_.compAttackMs);
    initSmooth(smCompRelease_, parameters_.compReleaseMs);
    initSmooth(smCompLowThresh_, parameters_.compLowThreshDb);
    initSmooth(smCompMidThresh_, parameters_.compMidThreshDb);
    initSmooth(smCompHighThresh_, parameters_.compHighThreshDb);
    initSmooth(smCrossoverLowFreq_, parameters_.crossoverLowFreq);
    initSmooth(smCrossoverHighFreq_, parameters_.crossoverHighFreq);
    initSmooth(smChainMix_, parameters_.chainMix);
    // A null chain-mix binding (direct test callers) means fully wet.
    if (parameters_.chainMix == nullptr) smChainMix_.setCurrentAndTargetValue(1.0f);

    // Stage switches start at their current state: a project loaded with a
    // stage on must not fade it in on the first block.
    const float fadeStep =
        1.0f / static_cast<float>(juce::jmax(1, juce::roundToInt(sampleRate_ * kStageFadeSeconds)));
    for (auto* fade : {&hpfFade_, &lpfFade_, &eqFade_, &eq2Fade_, &eq3Fade_, &compFade_, &multibandFade_})
        fade->step = fadeStep;
    hpfFade_.snap(loadSwitch(parameters_.hpfOn));
    lpfFade_.snap(loadSwitch(parameters_.lpfOn));
    eqFade_.snap(loadSwitch(parameters_.eqOn));
    eq2Fade_.snap(loadSwitch(parameters_.eq2On));
    eq3Fade_.snap(loadSwitch(parameters_.eq3On));
    compFade_.snap(loadSwitch(parameters_.compOn));
    multibandFade_.snap(loadSwitch(parameters_.compMultiband));

    advanceSmoothers(0);
    updateCoefficients(true);
    reset();
}

void CopilotCorrectiveDsp::applyCoefficientValues(StereoIIR& filter, const CoefficientValues& values) noexcept
{
    // This assignment normalises the factory's [b0, b1, b2, a0, a1, a2]
    // representation into JUCE's prepared state. Each state was constructed
    // with six slots, so its fixed-size assignment does not grow storage.
    *filter.state = values;
}

void CopilotCorrectiveDsp::updateCoefficients(bool force) noexcept
{
    using AC = juce::dsp::IIR::ArrayCoefficients<float>;
    const float nyquistGuard = static_cast<float>(sampleRate_ * 0.45);
    const auto freq = [nyquistGuard](const juce::SmoothedValue<float>& value) {
        return juce::jlimit(10.0f, nyquistGuard, value.getCurrentValue());
    };
    const auto q = [](const juce::SmoothedValue<float>& value) { return juce::jmax(0.05f, value.getCurrentValue()); };

    if (force || smHpfFreq_.isSmoothing()) applyCoefficientValues(hpf_, AC::makeHighPass(sampleRate_, freq(smHpfFreq_)));
    if (force || smLpfFreq_.isSmoothing()) applyCoefficientValues(lpf_, AC::makeLowPass(sampleRate_, freq(smLpfFreq_)));
    if (force || smEqFreq_.isSmoothing() || smEqGainDb_.isSmoothing() || smEqQ_.isSmoothing())
        applyCoefficientValues(peak_, AC::makePeakFilter(sampleRate_, freq(smEqFreq_), q(smEqQ_),
                                                         juce::Decibels::decibelsToGain(smEqGainDb_.getCurrentValue())));
    if (force || smEq2Freq_.isSmoothing() || smEq2GainDb_.isSmoothing() || smEq2Q_.isSmoothing())
        applyCoefficientValues(peak2_,
                               AC::makePeakFilter(sampleRate_, freq(smEq2Freq_), q(smEq2Q_),
                                                  juce::Decibels::decibelsToGain(smEq2GainDb_.getCurrentValue())));
    if (force || smEq3Freq_.isSmoothing() || smEq3GainDb_.isSmoothing() || smEq3Q_.isSmoothing())
        applyCoefficientValues(peak3_,
                               AC::makePeakFilter(sampleRate_, freq(smEq3Freq_), q(smEq3Q_),
                                                  juce::Decibels::decibelsToGain(smEq3GainDb_.getCurrentValue())));
}

void CopilotCorrectiveDsp::advanceSmoothers(int numSamples)
{
    const int n = juce::jmax(0, numSamples);
    // Returns true when the value moved (or will move) this sub-block, so the
    // matching coefficients are rebuilt - including the sub-block in which a
    // ramp finishes, which lands the filter exactly on its target.
    auto advance = [n](juce::SmoothedValue<float>& value, float target) {
        if (value.getTargetValue() != target) value.setTargetValue(target);
        const bool moving = value.isSmoothing();
        value.skip(n);
        return moving;
    };

    bool filtersMoved = false;
    filtersMoved |= advance(smHpfFreq_, loadParam(parameters_.hpfFreq));
    filtersMoved |= advance(smLpfFreq_, loadParam(parameters_.lpfFreq));
    filtersMoved |= advance(smEqFreq_, loadParam(parameters_.eqFreq));
    filtersMoved |= advance(smEqGainDb_, loadParam(parameters_.eqGainDb));
    filtersMoved |= advance(smEqQ_, loadParam(parameters_.eqQ));
    filtersMoved |= advance(smEq2Freq_, loadParam(parameters_.eq2Freq));
    filtersMoved |= advance(smEq2GainDb_, loadParam(parameters_.eq2GainDb));
    filtersMoved |= advance(smEq2Q_, loadParam(parameters_.eq2Q));
    filtersMoved |= advance(smEq3Freq_, loadParam(parameters_.eq3Freq));
    filtersMoved |= advance(smEq3GainDb_, loadParam(parameters_.eq3GainDb));
    filtersMoved |= advance(smEq3Q_, loadParam(parameters_.eq3Q));
    // A ramp that just finished reports isSmoothing() == false, so rebuild
    // every filter whenever any of them moved this sub-block.
    if (filtersMoved) updateCoefficients(true);

    advance(smCompThresh_, loadParam(parameters_.compThreshDb));
    advance(smCompRatio_, loadParam(parameters_.compRatio));
    advance(smCompAttack_, loadParam(parameters_.compAttackMs));
    advance(smCompRelease_, loadParam(parameters_.compReleaseMs));
    const float ratio = juce::jmax(1.0f, smCompRatio_.getCurrentValue());
    const float attack = juce::jmax(0.01f, smCompAttack_.getCurrentValue());
    const float release = juce::jmax(0.1f, smCompRelease_.getCurrentValue());
    comp_.setThreshold(smCompThresh_.getCurrentValue());
    comp_.setRatio(ratio);
    comp_.setAttack(attack);
    comp_.setRelease(release);

    advance(smCompLowThresh_, loadParam(parameters_.compLowThreshDb));
    advance(smCompMidThresh_, loadParam(parameters_.compMidThreshDb));
    advance(smCompHighThresh_, loadParam(parameters_.compHighThreshDb));
    advance(smCrossoverLowFreq_, loadParam(parameters_.crossoverLowFreq));
    advance(smCrossoverHighFreq_, loadParam(parameters_.crossoverHighFreq));
    crossover_.setCutoffs(smCrossoverLowFreq_.getCurrentValue(), smCrossoverHighFreq_.getCurrentValue());
    for (auto* band : {&compLow_, &compMid_, &compHigh_}) {
        band->setRatio(ratio);
        band->setAttack(attack);
        band->setRelease(release);
    }
    compLow_.setThreshold(smCompLowThresh_.getCurrentValue());
    compMid_.setThreshold(smCompMidThresh_.getCurrentValue());
    compHigh_.setThreshold(smCompHighThresh_.getCurrentValue());
}

void CopilotCorrectiveDsp::readStageTargets() noexcept
{
    // A stage that starts fading in from silence restarts its state, so it
    // does not replay history from the last time it ran.
    const auto target = [](StageFade& fade, bool on, auto&& resetFn) {
        if (on && fade.value <= 0.0f && fade.target <= 0.0f) resetFn();
        fade.target = on ? 1.0f : 0.0f;
    };
    target(hpfFade_, loadSwitch(parameters_.hpfOn), [this] { hpf_.reset(); });
    target(lpfFade_, loadSwitch(parameters_.lpfOn), [this] { lpf_.reset(); });
    target(eqFade_, loadSwitch(parameters_.eqOn), [this] { peak_.reset(); });
    target(eq2Fade_, loadSwitch(parameters_.eq2On), [this] { peak2_.reset(); });
    target(eq3Fade_, loadSwitch(parameters_.eq3On), [this] { peak3_.reset(); });
    target(compFade_, loadSwitch(parameters_.compOn), [this] {
        comp_.reset();
        compLow_.reset();
        compMid_.reset();
        compHigh_.reset();
        crossover_.reset();
    });
    const bool multiband = loadSwitch(parameters_.compMultiband);
    if (multiband && multibandFade_.value <= 0.0f && multibandFade_.target <= 0.0f) {
        compLow_.reset();
        compMid_.reset();
        compHigh_.reset();
        crossover_.reset();
    } else if (!multiband && multibandFade_.value >= 1.0f && multibandFade_.target >= 1.0f) {
        comp_.reset();
    }
    multibandFade_.target = multiband ? 1.0f : 0.0f;
}

void CopilotCorrectiveDsp::reset() noexcept
{
    hpf_.reset();
    lpf_.reset();
    peak_.reset();
    peak2_.reset();
    peak3_.reset();
    comp_.reset();
    crossover_.reset();
    compLow_.reset();
    compMid_.reset();
    compHigh_.reset();
    outGain_.reset();
    dryWet_.reset();
}

template <typename ProcessFn>
void CopilotCorrectiveDsp::runStage(StageFade& fade, juce::dsp::AudioBlock<float>& block, ProcessFn&& processFn)
{
    if (fade.silent()) return;
    if (fade.settledOn()) {
        processFn(block);
        return;
    }
    // Crossfading: keep the stage input, process, then blend per sample.
    const auto channels = block.getNumChannels();
    const auto n = block.getNumSamples();
    juce::dsp::AudioBlock<float> dry(stageDry_);
    auto drySub = dry.getSubsetChannelBlock(0, channels).getSubBlock(0, n);
    drySub.copyFrom(block);
    processFn(block);
    for (size_t i = 0; i < n; ++i) fadeGains_[i] = fade.next();
    for (size_t c = 0; c < channels; ++c) {
        float* out = block.getChannelPointer(c);
        const float* in = drySub.getChannelPointer(c);
        for (size_t i = 0; i < n; ++i) out[i] = in[i] + fadeGains_[i] * (out[i] - in[i]);
    }
}

void CopilotCorrectiveDsp::processCompressor(juce::dsp::AudioBlock<float>& block, bool multiband)
{
    if (!multiband) {
        juce::dsp::ProcessContextReplacing<float> context(block);
        comp_.process(context);
        return;
    }
    const auto channels = block.getNumChannels();
    const auto n = block.getNumSamples();
    juce::dsp::AudioBlock<float> lowBlock(lowBandBuffer_), midBlock(midBandBuffer_), highBlock(highBandBuffer_);
    // Process exactly n samples of the band buffers (they are sized for the
    // prepared maximum): running the crossover and band compressors over a
    // stale tail corrupted their state under pluginval's automation test.
    auto lowSub = lowBlock.getSubsetChannelBlock(0, channels).getSubBlock(0, n);
    auto midSub = midBlock.getSubsetChannelBlock(0, channels).getSubBlock(0, n);
    auto highSub = highBlock.getSubsetChannelBlock(0, channels).getSubBlock(0, n);
    juce::dsp::ProcessContextReplacing<float> context(block);
    crossover_.process(context, lowSub, midSub, highSub);
    juce::dsp::ProcessContextReplacing<float> lowContext(lowSub), midContext(midSub), highContext(highSub);
    compLow_.process(lowContext);
    compMid_.process(midContext);
    compHigh_.process(highContext);
    block.copyFrom(lowSub);
    block.add(midSub);
    block.add(highSub);
}

void CopilotCorrectiveDsp::processSubBlock(juce::dsp::AudioBlock<float> block)
{
    const int n = static_cast<int>(block.getNumSamples());
    readStageTargets();
    advanceSmoothers(n);

    auto iirStage = [](StereoIIR& filter) {
        return [&filter](juce::dsp::AudioBlock<float>& b) {
            juce::dsp::ProcessContextReplacing<float> context(b);
            filter.process(context);
        };
    };
    runStage(hpfFade_, block, iirStage(hpf_));
    runStage(lpfFade_, block, iirStage(lpf_));
    runStage(eqFade_, block, iirStage(peak_));
    runStage(eq2Fade_, block, iirStage(peak2_));
    runStage(eq3Fade_, block, iirStage(peak3_));
    runStage(compFade_, block, [this](juce::dsp::AudioBlock<float>& b) {
        if (multibandFade_.silent()) {
            processCompressor(b, false);
            return;
        }
        if (multibandFade_.settledOn()) {
            processCompressor(b, true);
            return;
        }
        // Single <-> multiband switch: run both and crossfade.
        const auto channels = b.getNumChannels();
        const auto samples = b.getNumSamples();
        juce::dsp::AudioBlock<float> alt(compAlt_);
        auto altSub = alt.getSubsetChannelBlock(0, channels).getSubBlock(0, samples);
        altSub.copyFrom(b);
        processCompressor(b, false);
        processCompressor(altSub, true);
        std::array<float, kControlBlockSamples> mix{};
        for (size_t i = 0; i < samples; ++i) mix[i] = multibandFade_.next();
        for (size_t c = 0; c < channels; ++c) {
            float* single = b.getChannelPointer(c);
            const float* multi = altSub.getChannelPointer(c);
            for (size_t i = 0; i < samples; ++i) single[i] += mix[i] * (multi[i] - single[i]);
        }
    });
    // The multiband ramp only advances while the compressor runs; when the
    // compressor is off, land the switch at its target immediately.
    if (compFade_.silent()) multibandFade_.value = multibandFade_.target;

    outGain_.setGainDecibels(loadParam(parameters_.outGainDb));
    juce::dsp::ProcessContextReplacing<float> context(block);
    outGain_.process(context);
}

void CopilotCorrectiveDsp::process(juce::AudioBuffer<float>& buffer, bool bypassed, int numSamples)
{
    // The DSP objects and dry/wet mixer were prepared for this maximum block
    // size. Do not resize or otherwise re-prepare from processBlock(); the
    // processor splits oversized host callbacks before they get here
    // (AUDIT-027), so this guard only protects direct callers.
    // A mismatched explicit count is likewise an invalid caller contract.
    if (bypassed || numSamples <= 0 || numSamples != buffer.getNumSamples()
        || numSamples > preparedBlockSize_)
        return;

    const auto channels = static_cast<size_t>(juce::jmin(buffer.getNumChannels(), preparedChannels_));
    juce::dsp::AudioBlock<float> block(buffer);
    auto processBlock = block.getSubsetChannelBlock(0, channels);

    smChainMix_.setTargetValue(parameters_.chainMix != nullptr ? loadParam(parameters_.chainMix) : 1.0f);
    smChainMix_.skip(numSamples);
    dryWet_.setWetMixProportion(smChainMix_.getCurrentValue());
    dryWet_.pushDrySamples(processBlock);

    for (int start = 0; start < numSamples; start += kControlBlockSamples) {
        const auto len = static_cast<size_t>(juce::jmin(kControlBlockSamples, numSamples - start));
        processSubBlock(processBlock.getSubBlock(static_cast<size_t>(start), len));
    }

    dryWet_.mixWetSamples(processBlock);
}

} // namespace khensu
