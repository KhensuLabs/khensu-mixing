// shared-core/include/khensu/dsp/LinkwitzRileyCrossover3Band.h
#pragma once
#include <juce_dsp/juce_dsp.h>
#include <cmath>

namespace khensu {

/**
    A phase-coherent 3-band Linkwitz-Riley (LR4) crossover filter.
    It splits a multi-channel or single-channel audio signal into Low, Mid, and High bands.
    
    To maintain perfect magnitude summation (flat frequency response):
    1. The input is split into Low and MidHigh bands using an LR4 crossover at lowCutoff.
    2. The MidHigh band is split into Mid and High bands using an LR4 crossover at highCutoff.
    3. The Low band is passed through an allpass filter at highCutoff to match the phase shift
       introduced to the MidHigh band by the second crossover.
*/
class LinkwitzRileyCrossover3Band {
  public:
    LinkwitzRileyCrossover3Band() = default;

    void prepare(const juce::dsp::ProcessSpec& spec)
    {
        sampleRate = spec.sampleRate;

        // Prepare Low/MidHigh crossover filters
        lowLP1.prepare(spec);
        lowLP2.prepare(spec);
        lowHP1.prepare(spec);
        lowHP2.prepare(spec);

        // Prepare Mid/High crossover filters
        midLP1.prepare(spec);
        midLP2.prepare(spec);
        midHP1.prepare(spec);
        midHP2.prepare(spec);

        // Prepare phase-matching allpass filters for the low band
        lowAP1.prepare(spec);

        updateCoefficients();
    }

    void reset()
    {
        lowLP1.reset();
        lowLP2.reset();
        lowHP1.reset();
        lowHP2.reset();

        midLP1.reset();
        midLP2.reset();
        midHP1.reset();
        midHP2.reset();

        lowAP1.reset();
    }

    void setCutoffs(float newLowCutoff, float newHighCutoff)
    {
        const float minCutoff = 20.0f;
        const float maxCutoff = sampleRate > 0.0 ? static_cast<float>(sampleRate * 0.49) : 20000.0f;

        auto clampedLow = juce::jlimit(minCutoff, maxCutoff - 20.0f, newLowCutoff);
        auto clampedHigh = juce::jlimit(clampedLow + 10.0f, maxCutoff, newHighCutoff);

        if (lowCutoff != clampedLow || highCutoff != clampedHigh) {
            lowCutoff = clampedLow;
            highCutoff = clampedHigh;
            updateCoefficients();
        }
    }

    float getLowCutoff() const noexcept { return lowCutoff; }
    float getHighCutoff() const noexcept { return highCutoff; }

    /**
        Splits the input audio block into low, mid, and high bands.
        The caller must ensure that the output blocks have the same size and channel structure
        as the input context.
    */
    void process(const juce::dsp::ProcessContextReplacing<float>& context, juce::dsp::AudioBlock<float>& lowBlock,
                 juce::dsp::AudioBlock<float>& midBlock, juce::dsp::AudioBlock<float>& highBlock)
    {
        const auto& inputBlock = context.getInputBlock();

        // 1. Process Low / MidHigh split on input
        // Low band lowpass path: LowLP1 -> LowLP2
        juce::dsp::ProcessContextReplacing<float> lowLPContext(lowBlock);
        lowBlock.copyFrom(inputBlock);
        lowLP1.process(lowLPContext);
        lowLP2.process(lowLPContext);

        // MidHigh band highpass path: LowHP1 -> LowHP2
        // We temporarily write MidHigh to midBlock, then split it
        juce::dsp::ProcessContextReplacing<float> midHighHPContext(midBlock);
        midBlock.copyFrom(inputBlock);
        lowHP1.process(midHighHPContext);
        lowHP2.process(midHighHPContext);

        // 2. Process Mid / High split on the MidHigh band (currently in midBlock)
        // High band highpass path: MidHP1 -> MidHP2
        juce::dsp::ProcessContextReplacing<float> highHPContext(highBlock);
        highBlock.copyFrom(midBlock);
        midHP1.process(highHPContext);
        midHP2.process(highHPContext);

        // Mid band lowpass path: MidLP1 -> MidLP2
        juce::dsp::ProcessContextReplacing<float> midLPContext(midBlock);
        midLP1.process(midLPContext);
        midLP2.process(midLPContext);

        // 3. Apply phase-matching allpass filter to Low band so it aligns with Mid & High
        // Low band allpass path: ONE 2nd-order allpass at highCutoff (Q = 1/sqrt2)
        juce::dsp::ProcessContextReplacing<float> lowAPContext(lowBlock);
        lowAP1.process(lowAPContext);
    }

  private:
    void updateCoefficients()
    {
        if (sampleRate <= 0.0) return;

        const float Q = 1.0f / std::sqrt(2.0f); // Butterworth Q

        // Two cascaded Butterworth lowpass filters make a 4th-order Linkwitz-Riley lowpass
        auto lowLPCoeffs = juce::dsp::IIR::ArrayCoefficients<float>::makeLowPass(sampleRate, lowCutoff, Q);
        *lowLP1.state = lowLPCoeffs;
        *lowLP2.state = lowLPCoeffs;

        // Two cascaded Butterworth highpass filters make a 4th-order Linkwitz-Riley highpass
        auto lowHPCoeffs = juce::dsp::IIR::ArrayCoefficients<float>::makeHighPass(sampleRate, lowCutoff, Q);
        *lowHP1.state = lowHPCoeffs;
        *lowHP2.state = lowHPCoeffs;

        // Two cascaded Butterworth lowpass filters for the mid-high crossover
        auto midLPCoeffs = juce::dsp::IIR::ArrayCoefficients<float>::makeLowPass(sampleRate, highCutoff, Q);
        *midLP1.state = midLPCoeffs;
        *midLP2.state = midLPCoeffs;

        // Two cascaded Butterworth highpass filters for the mid-high crossover
        auto midHPCoeffs = juce::dsp::IIR::ArrayCoefficients<float>::makeHighPass(sampleRate, highCutoff, Q);
        *midHP1.state = midHPCoeffs;
        *midHP2.state = midHPCoeffs;

        // LR4 LP + HP sums to exactly ONE 2nd-order allpass (Q = 1/sqrt2) at the
        // crossover frequency, so a single allpass matches the mid/high split's
        // phase. Two (as before) doubled the shift: -3.6 dB notches at 500/1000 Hz
        // and ~-39 dB with close cutoffs.
        auto lowAPCoeffs = juce::dsp::IIR::ArrayCoefficients<float>::makeAllPass(sampleRate, highCutoff, Q);
        *lowAP1.state = lowAPCoeffs;
    }

    double sampleRate = 44100.0;
    float lowCutoff = 200.0f;
    float highCutoff = 2500.0f;

    using Filter = juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>, juce::dsp::IIR::Coefficients<float>>;

    // Low / MidHigh crossover filters
    Filter lowLP1, lowLP2;
    Filter lowHP1, lowHP2;

    // Mid / High crossover filters
    Filter midLP1, midLP2;
    Filter midHP1, midHP2;

    // Phase-alignment allpass filters for low band
    Filter lowAP1;
};

} // namespace khensu
