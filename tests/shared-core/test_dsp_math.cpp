// tests/shared-core/test_dsp_math.cpp
// Regression tests for DSP-math defects found in the 2026-10-10 audit:
//  - LR4 3-band crossover must sum flat (one phase-matching allpass, not two)
//  - EQ curve peaking magnitude must equal the band gain at fc, tend to 0 dB
//    away from it, and never produce NaN
#include <catch2/catch_test_macros.hpp>
#include <khensu/dsp/LinkwitzRileyCrossover3Band.h>
#include <khensu/ui/EqCurveComponent.h>
#include <cmath>
#include <string>

namespace {
/// Steady-state gain (dB) of low+mid+high for a sine at `hz`.
double summedGainDb(float lowCut, float highCut, double hz, double sampleRate = 48000.0)
{
    const int n = 1 << 15;
    juce::dsp::ProcessSpec spec{sampleRate, static_cast<juce::uint32>(n), 1};
    khensu::LinkwitzRileyCrossover3Band xo;
    xo.prepare(spec);
    xo.setCutoffs(lowCut, highCut);
    juce::AudioBuffer<float> in(1, n), lo(1, n), mid(1, n), hi(1, n);
    for (int i = 0; i < n; ++i)
        in.setSample(0, i, static_cast<float>(std::sin(2.0 * 3.14159265358979323846 * hz * i / sampleRate)));
    juce::dsp::AudioBlock<float> inB(in), loB(lo), midB(mid), hiB(hi);
    juce::dsp::ProcessContextReplacing<float> ctx(inB);
    xo.process(ctx, loB, midB, hiB);
    // RMS ratio over the steady-state half (the input buffer was processed in
    // place by `ctx`, so regenerate the reference).
    double sumSq = 0.0, refSq = 0.0;
    for (int i = n / 2; i < n; ++i) {
        const double s = static_cast<double>(lo.getSample(0, i)) + mid.getSample(0, i) + hi.getSample(0, i);
        const double r = std::sin(2.0 * 3.14159265358979323846 * hz * i / sampleRate);
        sumSq += s * s;
        refSq += r * r;
    }
    return 10.0 * std::log10(sumSq / refSq);
}
} // namespace

TEST_CASE("3-band LR4 crossover sums flat across frequency and cutoff pairs", "[crossover][audit]")
{
    const std::pair<float, float> cutoffs[] = {{200.0f, 2500.0f}, {500.0f, 1000.0f}, {100.0f, 300.0f}, {1000.0f, 1010.0f}};
    for (const auto& [lo, hiC] : cutoffs) {
        for (double hz : {40.0, 150.0, 300.0, 606.0, 1000.0, 1500.0, 3000.0, 8000.0}) {
            INFO("cutoffs " << lo << "/" << hiC << " at " << hz << " Hz");
            // 0.1 dB: float biquads at fc/fs ~ 0.002 leave ~0.08 dB; the old
            // double-allpass bug was 3.6 dB (500/1000 Hz) to 39 dB (close cutoffs).
            CHECK(std::abs(summedGainDb(lo, hiC, hz)) < 0.1);
        }
    }
}

TEST_CASE("EQ curve peaking magnitude matches the analog peaking EQ", "[eqcurve][audit]")
{
    using khensu::EqCurveComponent;
    // Exactly the band gain at the centre frequency, for boosts and cuts.
    for (float g : {-12.0f, -6.0f, 3.0f, 6.0f, 12.0f})
        for (float q : {0.5f, 1.0f, 4.0f})
            CHECK(std::abs(EqCurveComponent::peakingMagnitudeDb(1000.0f, g, q, 1000.0f) - g) < 1e-3f);

    // Far from fc the band is flat; the response is finite everywhere and
    // symmetric on a log-frequency axis.
    CHECK(std::abs(EqCurveComponent::peakingMagnitudeDb(1000.0f, 6.0f, 1.0f, 20.0f)) < 0.05f);
    CHECK(std::abs(EqCurveComponent::peakingMagnitudeDb(1000.0f, 6.0f, 1.0f, 20000.0f)) < 0.2f);
    for (float hz = 20.0f; hz < 20000.0f; hz *= 1.1f)
        CHECK(std::isfinite(EqCurveComponent::peakingMagnitudeDb(1000.0f, 9.0f, 0.7f, hz)));
    const float below = EqCurveComponent::peakingMagnitudeDb(1000.0f, 6.0f, 1.0f, 500.0f);
    const float above = EqCurveComponent::peakingMagnitudeDb(1000.0f, 6.0f, 1.0f, 2000.0f);
    CHECK(std::abs(below - above) < 1e-3f);
    // A boost is never negative and never exceeds its gain.
    CHECK(below > 0.0f);
    CHECK(below < 6.0f);
    CHECK(EqCurveComponent::peakingMagnitudeDb(1000.0f, 6.0f, 0.0f, 1000.0f) == 0.0f); // Q guard
}

TEST_CASE("EQ curve draws shelves as shelves at the session rate (AUDIT-008)", "[eqcurve][audit]")
{
    using khensu::EqBand;
    using khensu::EqBandType;
    using khensu::EqCurveComponent;
    for (double fs : {44100.0, 48000.0, 96000.0}) {
        INFO("fs " << fs);
        EqBand hs;
        hs.hz = 5000.0f;
        hs.gain_db = 6.0f;
        hs.q = 0.707f;
        hs.type = EqBandType::HighShelf;
        CHECK(std::abs(EqCurveComponent::bandMagnitudeDb(hs, 20000.0f, fs) - 6.0f) < 0.5f);
        CHECK(std::abs(EqCurveComponent::bandMagnitudeDb(hs, 5000.0f, fs) - 3.0f) < 0.1f); // half gain at fc
        CHECK(std::abs(EqCurveComponent::bandMagnitudeDb(hs, 100.0f, fs)) < 0.05f);

        EqBand ls = hs;
        ls.type = EqBandType::LowShelf;
        ls.hz = 100.0f;
        ls.gain_db = -4.0f;
        CHECK(std::abs(EqCurveComponent::bandMagnitudeDb(ls, 20.0f, fs) - -4.0f) < 0.3f);
        CHECK(std::abs(EqCurveComponent::bandMagnitudeDb(ls, 100.0f, fs) - -2.0f) < 0.1f);
        CHECK(std::abs(EqCurveComponent::bandMagnitudeDb(ls, 10000.0f, fs)) < 0.05f);

        EqBand bell = hs;
        bell.type = EqBandType::Peak;
        CHECK(std::abs(EqCurveComponent::bandMagnitudeDb(bell, 5000.0f, fs) - 6.0f) < 0.01f);
        CHECK(std::abs(EqCurveComponent::bandMagnitudeDb(bell, 20.0f, fs)) < 0.05f);

        EqBand hpf;
        hpf.hz = 100.0f;
        hpf.q = 0.707f;
        hpf.type = EqBandType::HighPass;
        hpf.gain_db = 9.0f; // ignored by a pass filter
        CHECK(std::abs(EqCurveComponent::bandMagnitudeDb(hpf, 100.0f, fs) - -3.01f) < 0.1f); // Butterworth -3 dB
        CHECK(EqCurveComponent::bandMagnitudeDb(hpf, 25.0f, fs) < -20.0f);              // 12 dB/oct
        CHECK(std::abs(EqCurveComponent::bandMagnitudeDb(hpf, 5000.0f, fs)) < 0.05f);

        EqBand lpf = hpf;
        lpf.hz = 8000.0f;
        lpf.type = EqBandType::LowPass;
        CHECK(std::abs(EqCurveComponent::bandMagnitudeDb(lpf, 8000.0f, fs) - -3.01f) < 0.15f);
        CHECK(std::abs(EqCurveComponent::bandMagnitudeDb(lpf, 100.0f, fs)) < 0.05f);
    }
}

TEST_CASE("Recommended EQ shapes map to the render effect the service applies (AUDIT-008)", "[eqcurve][audit]")
{
    using namespace khensu;
    // SpectralRecommender's eq_type codes: 0 bell, 1 high shelf, 2 low shelf.
    CHECK(eqBandTypeFromCode(0.0f) == EqBandType::Peak);
    CHECK(eqBandTypeFromCode(1.0f) == EqBandType::HighShelf);
    CHECK(eqBandTypeFromCode(2.0f) == EqBandType::LowShelf);
    CHECK(eqBandTypeFromCode(3.0f) == EqBandType::HighPass);
    CHECK(eqBandTypeFromCode(4.0f) == EqBandType::LowPass);
    CHECK(eqBandTypeFromCode(42.0f) == EqBandType::Peak);
    // Effect names registered in local-services copilot/dsp.py EFFECT_MAP.
    CHECK(std::string(renderEffectName(EqBandType::Peak)) == "PeakFilter");
    CHECK(std::string(renderEffectName(EqBandType::HighShelf)) == "HighShelfFilter");
    CHECK(std::string(renderEffectName(EqBandType::LowShelf)) == "LowShelfFilter");
    CHECK(std::string(renderEffectName(EqBandType::HighPass)) == "HighpassFilter");
    CHECK(std::string(renderEffectName(EqBandType::LowPass)) == "LowpassFilter");
    CHECK_FALSE(eqBandTypeHasGain(EqBandType::HighPass));
    CHECK(eqBandTypeHasGain(EqBandType::HighShelf));
}
