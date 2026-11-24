// tests/plugin/PluginDeEsserShelfTest.cpp
// OVH-8: the corrective chain's EQ band shapes (bell / low shelf / high
// shelf) and the split-band de-esser.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "../../plugin-copilot/Source/CopilotCorrectiveDsp.h"

#include <cmath>

using khensu::CopilotCorrectiveDsp;

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kRate = 48000.0;
constexpr int kBlock = 512;

/// The atomics a CopilotCorrectiveDsp is bound to, at their plugin defaults.
struct Params {
    std::atomic<float> chainMix{1.0f}, hpfOn{0}, hpfFreq{80}, lpfOn{0}, lpfFreq{18000}, eqOn{0}, eqFreq{1000},
        eqGainDb{0}, eqQ{0.707f}, eq2On{0}, eq2Freq{1000}, eq2GainDb{0}, eq2Q{1}, eq3On{0}, eq3Freq{1000},
        eq3GainDb{0}, eq3Q{1}, compOn{0}, compThreshDb{-18}, compRatio{4}, compAttackMs{5}, compReleaseMs{100},
        compMultiband{0}, compLowThreshDb{-18}, compMidThreshDb{-18}, compHighThreshDb{-18},
        crossoverLowFreq{200}, crossoverHighFreq{2500}, outGainDb{0}, eqType{0}, eq2Type{0}, eq3Type{0},
        deessOn{0}, deessFreq{6000}, deessThreshDb{-30}, deessRangeDb{6};

    CopilotCorrectiveDsp::Parameters bind()
    {
        CopilotCorrectiveDsp::Parameters p;
        p.chainMix = &chainMix;
        p.hpfOn = &hpfOn;
        p.hpfFreq = &hpfFreq;
        p.lpfOn = &lpfOn;
        p.lpfFreq = &lpfFreq;
        p.eqOn = &eqOn;
        p.eqFreq = &eqFreq;
        p.eqGainDb = &eqGainDb;
        p.eqQ = &eqQ;
        p.eq2On = &eq2On;
        p.eq2Freq = &eq2Freq;
        p.eq2GainDb = &eq2GainDb;
        p.eq2Q = &eq2Q;
        p.eq3On = &eq3On;
        p.eq3Freq = &eq3Freq;
        p.eq3GainDb = &eq3GainDb;
        p.eq3Q = &eq3Q;
        p.compOn = &compOn;
        p.compThreshDb = &compThreshDb;
        p.compRatio = &compRatio;
        p.compAttackMs = &compAttackMs;
        p.compReleaseMs = &compReleaseMs;
        p.compMultiband = &compMultiband;
        p.compLowThreshDb = &compLowThreshDb;
        p.compMidThreshDb = &compMidThreshDb;
        p.compHighThreshDb = &compHighThreshDb;
        p.crossoverLowFreq = &crossoverLowFreq;
        p.crossoverHighFreq = &crossoverHighFreq;
        p.outGainDb = &outGainDb;
        p.eqType = &eqType;
        p.eq2Type = &eq2Type;
        p.eq3Type = &eq3Type;
        p.deessOn = &deessOn;
        p.deessFreq = &deessFreq;
        p.deessThreshDb = &deessThreshDb;
        p.deessRangeDb = &deessRangeDb;
        return p;
    }
};

struct Result {
    double inRms = 0.0, outRms = 0.0, maxDiff = 0.0;
    bool finite = true;
    double gainDb() const { return 20.0 * std::log10(outRms / inRms); }
};

/// Runs `seconds` of a stereo sine at `hz` / `amp` through `dsp`; measures
/// the last half (after smoothing and fades settle).
Result runSine(CopilotCorrectiveDsp& dsp, double hz, float amp, double seconds = 0.6)
{
    Result r;
    const int blocks = static_cast<int>(seconds * kRate / kBlock);
    juce::AudioBuffer<float> buffer(2, kBlock);
    long long n = 0;
    double inSq = 0.0, outSq = 0.0;
    long long counted = 0;
    for (int b = 0; b < blocks; ++b) {
        std::vector<float> input(static_cast<size_t>(kBlock));
        for (int i = 0; i < kBlock; ++i) {
            const auto v = static_cast<float>(amp * std::sin(2.0 * kPi * hz * static_cast<double>(n + i) / kRate));
            input[static_cast<size_t>(i)] = v;
            buffer.setSample(0, i, v);
            buffer.setSample(1, i, v);
        }
        n += kBlock;
        dsp.process(buffer, false, kBlock);
        if (b < blocks / 2) continue;
        for (int i = 0; i < kBlock; ++i) {
            const double x = input[static_cast<size_t>(i)], y = buffer.getSample(0, i);
            r.finite = r.finite && std::isfinite(y) && std::isfinite(buffer.getSample(1, i));
            inSq += x * x;
            outSq += y * y;
            r.maxDiff = juce::jmax(r.maxDiff, std::abs(x - y));
            ++counted;
        }
    }
    r.inRms = std::sqrt(inSq / static_cast<double>(counted));
    r.outRms = std::sqrt(outSq / static_cast<double>(counted));
    return r;
}

struct Rig {
    Params params;
    CopilotCorrectiveDsp dsp;
    void prepare()
    {
        dsp.bindParameters(params.bind());
        dsp.prepareToPlay(kRate, kBlock, 2);
    }
};
} // namespace

TEST_CASE("A low-shelf band moves the lows and leaves the highs (OVH-8)", "[plugin][ovh8][shelf]")
{
    Rig rig;
    rig.params.eqOn = 1.0f;
    rig.params.eqType = 1.0f; // low shelf
    rig.params.eqFreq = 200.0f;
    rig.params.eqGainDb = -6.0f;
    rig.prepare();
    CHECK(runSine(rig.dsp, 40.0, 0.25f).gainDb() == Catch::Approx(-6.0).margin(0.4));
    CHECK(runSine(rig.dsp, 6000.0, 0.25f).gainDb() == Catch::Approx(0.0).margin(0.2));

    // The same band as a bell at 200 Hz barely touches 40 Hz.
    rig.params.eqType = 0.0f;
    CHECK(runSine(rig.dsp, 40.0, 0.25f).gainDb() > -3.0);
}

TEST_CASE("A high-shelf band lifts the highs and leaves the lows (OVH-8)", "[plugin][ovh8][shelf]")
{
    Rig rig;
    rig.params.eq2On = 1.0f;
    rig.params.eq2Type = 2.0f; // high shelf
    rig.params.eq2Freq = 4000.0f;
    rig.params.eq2GainDb = 6.0f;
    rig.params.eq2Q = 0.707f;
    rig.prepare();
    CHECK(runSine(rig.dsp, 15000.0, 0.1f).gainDb() == Catch::Approx(6.0).margin(0.5));
    CHECK(runSine(rig.dsp, 100.0, 0.1f).gainDb() == Catch::Approx(0.0).margin(0.2));
}

TEST_CASE("The de-esser turns down sibilance and leaves low content bit-exact (OVH-8)", "[plugin][ovh8][deess]")
{
    Rig rig;
    rig.params.deessOn = 1.0f;
    rig.params.deessFreq = 6000.0f;
    rig.params.deessThreshDb = -30.0f;
    rig.params.deessRangeDb = 6.0f;
    rig.prepare();

    // An 8 kHz tone at -12 dBFS is 18 dB over the threshold: 4:1 asks for
    // 13.5 dB, the range caps it at 6 dB.
    const auto sibilant = runSine(rig.dsp, 8000.0, 0.25f);
    CHECK(sibilant.finite);
    CHECK(rig.dsp.lastDeessReductionDb() == Catch::Approx(6.0f).margin(0.05f));
    CHECK(sibilant.gainDb() < -3.0);
    CHECK(sibilant.gainDb() > -7.5);

    // 200 Hz at the same level never reaches the detector (high band only).
    const auto low = runSine(rig.dsp, 200.0, 0.25f);
    CHECK(rig.dsp.lastDeessReductionDb() == 0.0f);
    CHECK(low.maxDiff == 0.0); // x + high * (g - 1) with g = 1 is x

    // Below the threshold: nothing either.
    const auto quiet = runSine(rig.dsp, 8000.0, 0.01f);
    CHECK(rig.dsp.lastDeessReductionDb() == 0.0f);
    CHECK(quiet.maxDiff == 0.0);

    // Range 0 disables the reduction.
    rig.params.deessRangeDb = 0.0f;
    runSine(rig.dsp, 8000.0, 0.25f);
    CHECK(rig.dsp.lastDeessReductionDb() == 0.0f);
}

TEST_CASE("The de-esser stays finite under random automation and switching (OVH-8)", "[plugin][ovh8][deess]")
{
    Rig rig;
    rig.prepare();
    juce::Random rng(77);
    juce::AudioBuffer<float> buffer(2, 256);
    bool finite = true;
    for (int it = 0; it < 400 && finite; ++it) {
        rig.params.deessOn = rng.nextBool() ? 1.0f : 0.0f;
        rig.params.deessFreq = 2000.0f + rng.nextFloat() * 10000.0f;
        rig.params.deessThreshDb = -60.0f * rng.nextFloat();
        rig.params.deessRangeDb = 24.0f * rng.nextFloat();
        rig.params.eqType = static_cast<float>(rng.nextInt(3));
        rig.params.eqOn = 1.0f;
        rig.params.eqGainDb = rng.nextFloat() * 24.0f - 12.0f;
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < 256; ++i) buffer.setSample(c, i, rng.nextFloat() * 2.0f - 1.0f);
        rig.dsp.process(buffer, false, 256);
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < 256; ++i) finite = finite && std::isfinite(buffer.getSample(c, i));
    }
    CHECK(finite);
}
