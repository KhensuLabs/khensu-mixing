#include "CopilotCorrectiveDsp.h"

namespace khensu {

namespace {
float loadParam(const std::atomic<float>* value) noexcept
{
    return value != nullptr ? value->load(std::memory_order_relaxed) : 0.0f;
}

bool loadSwitch(const std::atomic<float>* value) noexcept { return loadParam(value) > 0.5f; }

/// A null binding (direct test callers) falls back to `fallback`.
float loadOr(const std::atomic<float>* value, float fallback) noexcept
{
    return value != nullptr ? value->load(std::memory_order_relaxed) : fallback;
}

// A band 4-6 with nothing bound sits at the plugin's defaults.
constexpr float kDefaultBandFreq = 1000.0f;
constexpr float kDefaultBandQ = 1.0f;
} // namespace

void CopilotCorrectiveDsp::bindParameters(const Parameters& parameters)
{
    parameters_ = parameters;
    const std::atomic<float>* legacy[3][5] = {
        {parameters.eqOn, parameters.eqFreq, parameters.eqGainDb, parameters.eqQ, parameters.eqType},
        {parameters.eq2On, parameters.eq2Freq, parameters.eq2GainDb, parameters.eq2Q, parameters.eq2Type},
        {parameters.eq3On, parameters.eq3Freq, parameters.eq3GainDb, parameters.eq3Q, parameters.eq3Type}};
    for (size_t i = 0; i < bound_.size(); ++i) {
        const auto& ext = parameters.bands[i];
        auto& b = bound_[i];
        b.ext = ext;
        const auto pick = [&](std::atomic<float>* own, int field) -> const std::atomic<float>* {
            if (own != nullptr || i >= 3) return own;
            return legacy[i][field];
        };
        b.on = pick(ext.on, 0);
        b.freq = pick(ext.freq, 1);
        b.gainDb = pick(ext.gainDb, 2);
        b.q = pick(ext.q, 3);
        b.type = pick(ext.type, 4);
    }
}

int CopilotCorrectiveDsp::bandType(int index) const noexcept
{
    const auto& b = bound_[static_cast<size_t>(index)];
    // Kind 1..3 = notch / high-pass / low-pass; 0 = the v3 shape.
    const int kind = juce::jlimit(0, 3, juce::roundToInt(loadParam(b.ext.kind)));
    if (kind > 0) return 2 + kind;
    return juce::jlimit(0, 2, juce::roundToInt(loadParam(b.type)));
}

int CopilotCorrectiveDsp::bandMode(int index) const noexcept
{
    return juce::jlimit(0, 2, juce::roundToInt(loadParam(bound_[static_cast<size_t>(index)].ext.mode)));
}

dsp::RealtimeEqBand::Dynamics CopilotCorrectiveDsp::bandDynamics(int index) const noexcept
{
    const auto& ext = bound_[static_cast<size_t>(index)].ext;
    dsp::RealtimeEqBand::Dynamics d;
    d.on = loadSwitch(ext.dynOn);
    d.thresholdDb = loadOr(ext.dynThreshDb, -30.0f);
    d.rangeDb = juce::jmax(0.0f, loadOr(ext.dynRangeDb, 6.0f));
    d.attackMs = loadOr(ext.dynAttackMs, 5.0f);
    d.releaseMs = loadOr(ext.dynReleaseMs, 120.0f);
    return d;
}

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
    for (auto& band : bands_) band.prepare(sampleRate_, preparedChannels_);
    transient_.prepare(sampleRate_);
    saturation_.prepare(sampleRate_);
    bassMono_.prepare(sampleRate_);
    deessSplit_.prepare(spec);
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
    deessHigh_.setSize(preparedChannels_, kControlBlockSamples);
    deessAttack_ = static_cast<float>(1.0 - std::exp(-1.0 / (kDeessAttackSeconds * sampleRate_)));
    deessRelease_ = static_cast<float>(1.0 - std::exp(-1.0 / (kDeessReleaseSeconds * sampleRate_)));
    outGain_.prepare(spec);
    dryWet_.prepare(spec);
    dryWet_.setMixingRule(juce::dsp::DryWetMixingRule::linear);
    outGain_.setRampDurationSeconds(kParamSmoothingSeconds);

    auto initSmooth = [this](juce::SmoothedValue<float>& value, float start) {
        value.reset(sampleRate_, kParamSmoothingSeconds);
        value.setCurrentAndTargetValue(start);
    };
    initSmooth(smHpfFreq_, loadParam(parameters_.hpfFreq));
    initSmooth(smLpfFreq_, loadParam(parameters_.lpfFreq));
    for (size_t i = 0; i < bound_.size(); ++i) {
        // Bands 1-3 keep their v4 behaviour (an unbound value reads 0).
        const bool legacy = i < 3;
        initSmooth(smBandFreq_[i], legacy ? loadParam(bound_[i].freq) : loadOr(bound_[i].freq, kDefaultBandFreq));
        initSmooth(smBandGain_[i], loadParam(bound_[i].gainDb));
        initSmooth(smBandQ_[i], legacy ? loadParam(bound_[i].q) : loadOr(bound_[i].q, kDefaultBandQ));
    }
    initSmooth(smCompThresh_, loadParam(parameters_.compThreshDb));
    initSmooth(smCompRatio_, loadParam(parameters_.compRatio));
    initSmooth(smCompAttack_, loadParam(parameters_.compAttackMs));
    initSmooth(smCompRelease_, loadParam(parameters_.compReleaseMs));
    initSmooth(smCompLowThresh_, loadParam(parameters_.compLowThreshDb));
    initSmooth(smCompMidThresh_, loadParam(parameters_.compMidThreshDb));
    initSmooth(smCompHighThresh_, loadParam(parameters_.compHighThreshDb));
    initSmooth(smCrossoverLowFreq_, loadParam(parameters_.crossoverLowFreq));
    initSmooth(smCrossoverHighFreq_, loadParam(parameters_.crossoverHighFreq));
    // A null chain-mix binding (direct test callers) means fully wet.
    initSmooth(smChainMix_, loadOr(parameters_.chainMix, 1.0f));
    initSmooth(smDeessFreq_, loadOr(parameters_.deessFreq, 6500.0f));
    initSmooth(smDeessThresh_, loadOr(parameters_.deessThreshDb, -30.0f));
    initSmooth(smDeessRange_, loadOr(parameters_.deessRangeDb, 6.0f));
    initSmooth(smTsAttack_, loadOr(parameters_.tsAttackDb, 0.0f));
    initSmooth(smTsSustain_, loadOr(parameters_.tsSustainDb, 0.0f));
    initSmooth(smSatDrive_, loadOr(parameters_.satDriveDb, 6.0f));
    initSmooth(smSatWarmth_, loadOr(parameters_.satWarmth, 0.5f));
    initSmooth(smSatMix_, loadOr(parameters_.satMix, 1.0f));
    initSmooth(smSatTrim_, loadOr(parameters_.satTrimDb, 0.0f));
    initSmooth(smBassMonoFreq_, loadOr(parameters_.bassMonoFreq, dsp::RealtimeBassMono::kDefaultCrossoverHz));
    initSmooth(smWidth_, loadOr(parameters_.widthAmount, 1.0f));
    width_.reset(smWidth_.getCurrentValue());
    for (int i = 0; i < kNumBands; ++i) {
        bandType_[static_cast<size_t>(i)] = bandType(i);
        bandMode_[static_cast<size_t>(i)] = bandMode(i);
        bands_[static_cast<size_t>(i)].setDynamics(bandDynamics(i));
    }

    // Stage switches start at their current state: a project loaded with a
    // stage on must not fade it in on the first block.
    const float fadeStep =
        1.0f / static_cast<float>(juce::jmax(1, juce::roundToInt(sampleRate_ * kStageFadeSeconds)));
    for (auto* fade : {&hpfFade_, &lpfFade_, &compFade_, &multibandFade_, &deessFade_, &tsFade_, &satFade_,
                       &bassMonoFade_, &widthFade_})
        fade->step = fadeStep;
    for (auto& fade : bandFades_) fade.step = fadeStep;
    hpfFade_.snap(loadSwitch(parameters_.hpfOn));
    lpfFade_.snap(loadSwitch(parameters_.lpfOn));
    for (size_t i = 0; i < bandFades_.size(); ++i) bandFades_[i].snap(loadSwitch(bound_[i].on));
    compFade_.snap(loadSwitch(parameters_.compOn));
    multibandFade_.snap(loadSwitch(parameters_.compMultiband));
    deessFade_.snap(loadSwitch(parameters_.deessOn));
    tsFade_.snap(loadSwitch(parameters_.tsOn));
    satFade_.snap(loadSwitch(parameters_.satOn));
    bassMonoFade_.snap(loadSwitch(parameters_.bassMonoOn));
    widthFade_.snap(loadSwitch(parameters_.widthOn));

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

    if (force || smHpfFreq_.isSmoothing()) applyCoefficientValues(hpf_, AC::makeHighPass(sampleRate_, freq(smHpfFreq_)));
    if (force || smLpfFreq_.isSmoothing()) applyCoefficientValues(lpf_, AC::makeLowPass(sampleRate_, freq(smLpfFreq_)));
    // Each band (OVH-8 shapes, STU-10 kinds / channels); the band clamps the
    // frequency and Q exactly as the v4 bands did and re-designs only when
    // something changed.
    for (size_t i = 0; i < bands_.size(); ++i)
        bands_[i].setBand(static_cast<dsp::RealtimeEqBand::Type>(bandType_[i]),
                          static_cast<dsp::RealtimeEqBand::Channels>(bandMode_[i]), smBandFreq_[i].getCurrentValue(),
                          smBandQ_[i].getCurrentValue(), smBandGain_[i].getCurrentValue(), force);
    if (force || smDeessFreq_.isSmoothing())
        applyCoefficientValues(deessSplit_, AC::makeLowPass(sampleRate_, freq(smDeessFreq_), 0.7071f));
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
    for (size_t i = 0; i < bound_.size(); ++i) {
        const bool legacy = i < 3;
        filtersMoved |= advance(smBandFreq_[i], legacy ? loadParam(bound_[i].freq)
                                                       : loadOr(bound_[i].freq, kDefaultBandFreq));
        filtersMoved |= advance(smBandGain_[i], loadParam(bound_[i].gainDb));
        filtersMoved |= advance(smBandQ_[i], legacy ? loadParam(bound_[i].q) : loadOr(bound_[i].q, kDefaultBandQ));
    }
    filtersMoved |= advance(smDeessFreq_, loadOr(parameters_.deessFreq, 6500.0f));
    // A type or channel change rebuilds the band at once (a switch, not a ramp).
    for (int i = 0; i < kNumBands; ++i) {
        const auto k = static_cast<size_t>(i);
        const int type = bandType(i), mode = bandMode(i);
        if (type != bandType_[k] || mode != bandMode_[k]) {
            bandType_[k] = type;
            bandMode_[k] = mode;
            filtersMoved = true;
        }
        bands_[k].setDynamics(bandDynamics(i));
    }
    // A ramp that just finished reports isSmoothing() == false, so rebuild
    // every filter whenever any of them moved this sub-block.
    if (filtersMoved) updateCoefficients(true);

    advance(smDeessThresh_, loadOr(parameters_.deessThreshDb, -30.0f));
    advance(smDeessRange_, juce::jmax(0.0f, loadOr(parameters_.deessRangeDb, 6.0f)));
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

    // STU-10 stages.
    advance(smTsAttack_, loadOr(parameters_.tsAttackDb, 0.0f));
    advance(smTsSustain_, loadOr(parameters_.tsSustainDb, 0.0f));
    advance(smSatDrive_, loadOr(parameters_.satDriveDb, 6.0f));
    advance(smSatWarmth_, loadOr(parameters_.satWarmth, 0.5f));
    advance(smSatMix_, loadOr(parameters_.satMix, 1.0f));
    advance(smSatTrim_, loadOr(parameters_.satTrimDb, 0.0f));
    advance(smBassMonoFreq_, loadOr(parameters_.bassMonoFreq, dsp::RealtimeBassMono::kDefaultCrossoverHz));
    advance(smWidth_, juce::jlimit(0.0f, 2.0f, loadOr(parameters_.widthAmount, 1.0f)));
    bassMono_.setCrossover(smBassMonoFreq_.getCurrentValue());
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
    for (size_t i = 0; i < bandFades_.size(); ++i)
        target(bandFades_[i], loadSwitch(bound_[i].on), [this, i] { bands_[i].reset(); });
    target(deessFade_, loadSwitch(parameters_.deessOn), [this] {
        deessSplit_.reset();
        deessEnv_ = 0.0f;
    });
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
    target(tsFade_, loadSwitch(parameters_.tsOn), [this] { transient_.reset(); });
    target(satFade_, loadSwitch(parameters_.satOn), [this] { saturation_.reset(); });
    target(bassMonoFade_, loadSwitch(parameters_.bassMonoOn), [this] { bassMono_.reset(); });
    target(widthFade_, loadSwitch(parameters_.widthOn), [this] { width_.reset(smWidth_.getCurrentValue()); });
}

void CopilotCorrectiveDsp::reset() noexcept
{
    hpf_.reset();
    lpf_.reset();
    for (auto& band : bands_) band.reset();
    deessSplit_.reset();
    deessEnv_ = 0.0f;
    comp_.reset();
    crossover_.reset();
    compLow_.reset();
    compMid_.reset();
    compHigh_.reset();
    transient_.reset();
    saturation_.reset();
    bassMono_.reset();
    width_.reset(smWidth_.getCurrentValue());
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

void CopilotCorrectiveDsp::processDeEsser(juce::dsp::AudioBlock<float>& block) noexcept
{
    const auto channels = block.getNumChannels();
    const auto n = block.getNumSamples();
    juce::dsp::AudioBlock<float> lowAll(deessHigh_);
    auto low = lowAll.getSubsetChannelBlock(0, channels).getSubBlock(0, n);
    low.copyFrom(block);
    {
        juce::dsp::ProcessContextReplacing<float> context(low);
        deessSplit_.process(context);
    }
    const float threshold = smDeessThresh_.getCurrentValue();
    const float range = smDeessRange_.getCurrentValue();
    const float slope = 1.0f - 1.0f / kDeessRatio;
    for (size_t i = 0; i < n; ++i) {
        // Linked detector: the louder channel's high band (x - LP).
        float peak = 0.0f;
        for (size_t c = 0; c < channels; ++c)
            peak = juce::jmax(peak, std::abs(block.getChannelPointer(c)[i] - low.getChannelPointer(c)[i]));
        deessEnv_ += (peak > deessEnv_ ? deessAttack_ : deessRelease_) * (peak - deessEnv_);
        const float levelDb = 20.0f * std::log10(deessEnv_ + 1.0e-9f);
        const float gr = juce::jlimit(0.0f, range, (levelDb - threshold) * slope);
        if (gr <= 0.0f) continue; // idle: the input passes untouched
        deessBlockMaxGr_ = juce::jmax(deessBlockMaxGr_, gr);
        const float g = std::exp(-gr * 0.11512925f); // dB -> gain
        for (size_t c = 0; c < channels; ++c) {
            float& x = block.getChannelPointer(c)[i];
            const float lp = low.getChannelPointer(c)[i];
            x = lp + g * (x - lp);
        }
    }
    // A non-finite input (contained by the processor) must not stick here.
    if (!std::isfinite(deessEnv_)) deessEnv_ = 0.0f;
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
    // The block's channels as a pointer table, for the shared-core stages.
    const auto withChannels = [](juce::dsp::AudioBlock<float>& b, auto&& fn) {
        std::array<float*, dsp::kRealtimeMaxChannels> ptrs{};
        const int nch = static_cast<int>(juce::jmin(b.getNumChannels(), ptrs.size()));
        for (int c = 0; c < nch; ++c) ptrs[static_cast<size_t>(c)] = b.getChannelPointer(static_cast<size_t>(c));
        fn(ptrs.data(), nch, static_cast<int>(b.getNumSamples()));
    };
    runStage(hpfFade_, block, iirStage(hpf_));
    runStage(lpfFade_, block, iirStage(lpf_));
    for (size_t i = 0; i < bands_.size(); ++i)
        runStage(bandFades_[i], block, [this, i, &withChannels](juce::dsp::AudioBlock<float>& b) {
            withChannels(b, [this, i](float* const* ch, int nch, int ns) { bands_[i].process(ch, nch, ns); });
        });
    runStage(deessFade_, block, [this](juce::dsp::AudioBlock<float>& b) { processDeEsser(b); });
    bool compRan = false;
    runStage(compFade_, block, [this, &compRan](juce::dsp::AudioBlock<float>& b) {
        // VIZ-6b: the stage's reduction = input power / output power (the
        // compressor has no make-up gain), and its input level.
        compRan = true;
        double inSq = 0.0;
        const auto nch = b.getNumChannels(), ns = b.getNumSamples();
        for (size_t ch = 0; ch < nch; ++ch) {
            const float* x = b.getChannelPointer(ch);
            for (size_t i = 0; i < ns; ++i) inSq += static_cast<double>(x[i]) * x[i];
        }
        struct Measure {
            CopilotCorrectiveDsp& self;
            juce::dsp::AudioBlock<float>& blk;
            double in;
            ~Measure()
            {
                double outSq = 0.0;
                for (size_t ch = 0; ch < blk.getNumChannels(); ++ch) {
                    const float* y = blk.getChannelPointer(ch);
                    for (size_t i = 0; i < blk.getNumSamples(); ++i) outSq += static_cast<double>(y[i]) * y[i];
                }
                const double count = static_cast<double>(blk.getNumChannels() * blk.getNumSamples());
                const float grDb = in > 1e-12 && outSq > 1e-12 ? static_cast<float>(10.0 * std::log10(in / outSq)) : 0.0f;
                self.compReductionDb_.store(juce::jmax(0.0f, grDb), std::memory_order_relaxed);
                self.compInputDb_.store(count > 0.0 && in > 1e-12 ? static_cast<float>(10.0 * std::log10(in / count)) : -120.0f,
                                        std::memory_order_relaxed);
            }
        } measure{*this, b, inSq};
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
    if (!compRan) {
        compReductionDb_.store(0.0f, std::memory_order_relaxed);
        compInputDb_.store(-120.0f, std::memory_order_relaxed);
    }
    // The multiband ramp only advances while the compressor runs; when the
    // compressor is off, land the switch at its target immediately.
    if (compFade_.silent()) multibandFade_.value = multibandFade_.target;

    // STU-10: colour, then space.
    runStage(tsFade_, block, [this, &withChannels](juce::dsp::AudioBlock<float>& b) {
        dsp::RealtimeTransientShaper::Settings s;
        s.attackGainDb = smTsAttack_.getCurrentValue();
        s.sustainGainDb = smTsSustain_.getCurrentValue();
        s.attackMs = loadOr(parameters_.tsAttackMs, 3.0f);
        s.releaseMs = loadOr(parameters_.tsReleaseMs, 200.0f);
        withChannels(b, [this, &s](float* const* ch, int nch, int ns) { transient_.process(ch, nch, ns, s); });
    });
    runStage(satFade_, block, [this, &withChannels](juce::dsp::AudioBlock<float>& b) {
        dsp::RealtimeSaturation::Settings s;
        s.driveDb = smSatDrive_.getCurrentValue();
        s.warmth = smSatWarmth_.getCurrentValue();
        s.mix = smSatMix_.getCurrentValue();
        s.trimDb = smSatTrim_.getCurrentValue();
        withChannels(b, [this, &s](float* const* ch, int nch, int ns) { saturation_.process(ch, nch, ns, s); });
    });
    runStage(bassMonoFade_, block, [this, &withChannels](juce::dsp::AudioBlock<float>& b) {
        withChannels(b, [this](float* const* ch, int nch, int ns) { bassMono_.process(ch, nch, ns); });
    });
    runStage(widthFade_, block, [this, &withChannels](juce::dsp::AudioBlock<float>& b) {
        const float w = smWidth_.getCurrentValue();
        withChannels(b, [this, w](float* const* ch, int nch, int ns) { width_.process(ch, nch, ns, w); });
    });

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
    deessBlockMaxGr_ = 0.0f;

    for (int start = 0; start < numSamples; start += kControlBlockSamples) {
        const auto len = static_cast<size_t>(juce::jmin(kControlBlockSamples, numSamples - start));
        processSubBlock(processBlock.getSubBlock(static_cast<size_t>(start), len));
    }

    dryWet_.mixWetSamples(processBlock);
    deessReductionDb_.store(deessBlockMaxGr_, std::memory_order_relaxed);
}

} // namespace khensu
