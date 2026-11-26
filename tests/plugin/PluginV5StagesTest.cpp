// tests/plugin/PluginV5StagesTest.cpp
// STU-10: the plugin's corrective chain at parity with the desktop renderer -
// six EQ bands with kinds, channels and a dynamic mode, the transient shaper,
// saturation, bass mono and width stages (state v5), the v4 -> v5 migration,
// allocation-free processing with every stage on, and "Apply all safe fixes"
// mapping the width, bass-mono, tonal + resonance and masking fixes.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "../../plugin-copilot/Source/CopilotCorrectiveDsp.h"
#include "../../plugin-copilot/Source/FixBatch.h"
#include "../../plugin-copilot/Source/PluginProcessor.h"
#include "../../plugin-copilot/Source/ui/DspControlGridPanel.h"
#include "AllocationGuard.h"

#include <khensu/mixerchain/RecommendationDspMapper.h>

#include <cmath>
#include <set>

using khensu::CopilotCorrectiveDsp;
using khensu::KhensuCopilotProcessor;
using khensu::Recommendation;
using P = khensu::KhensuCopilotProcessor::ParamIDs;
namespace mc = khensu::mixerchain;

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kRate = 48000.0;
constexpr int kBlock = 512;

/// Every atomic a CopilotCorrectiveDsp is bound to, at the plugin defaults.
struct Params {
    std::atomic<float> chainMix{1.0f}, hpfOn{0}, hpfFreq{80}, lpfOn{0}, lpfFreq{18000}, compOn{0}, compThreshDb{-18},
        compRatio{4}, compAttackMs{5}, compReleaseMs{100}, compMultiband{0}, compLowThreshDb{-18},
        compMidThreshDb{-18}, compHighThreshDb{-18}, crossoverLowFreq{200}, crossoverHighFreq{2500}, outGainDb{0},
        deessOn{0}, deessFreq{6500}, deessThreshDb{-30}, deessRangeDb{6};
    struct Band {
        std::atomic<float> on{0}, freq{1000}, gainDb{0}, q{1}, type{0}, kind{0}, mode{0}, dynOn{0}, dynThreshDb{-30},
            dynRangeDb{6}, dynAttackMs{5}, dynReleaseMs{120};
    };
    std::array<Band, 6> bands;
    std::atomic<float> tsOn{0}, tsAttackDb{0}, tsSustainDb{0}, tsAttackMs{3}, tsReleaseMs{200}, satOn{0},
        satDriveDb{6}, satWarmth{0.5f}, satMix{1}, satTrimDb{0}, bassMonoOn{0}, bassMonoFreq{120}, widthOn{0},
        widthAmount{1};

    CopilotCorrectiveDsp::Parameters bind()
    {
        CopilotCorrectiveDsp::Parameters p;
        p.chainMix = &chainMix;
        p.hpfOn = &hpfOn;
        p.hpfFreq = &hpfFreq;
        p.lpfOn = &lpfOn;
        p.lpfFreq = &lpfFreq;
        p.eqOn = &bands[0].on;
        p.eqFreq = &bands[0].freq;
        p.eqGainDb = &bands[0].gainDb;
        p.eqQ = &bands[0].q;
        p.eqType = &bands[0].type;
        p.eq2On = &bands[1].on;
        p.eq2Freq = &bands[1].freq;
        p.eq2GainDb = &bands[1].gainDb;
        p.eq2Q = &bands[1].q;
        p.eq2Type = &bands[1].type;
        p.eq3On = &bands[2].on;
        p.eq3Freq = &bands[2].freq;
        p.eq3GainDb = &bands[2].gainDb;
        p.eq3Q = &bands[2].q;
        p.eq3Type = &bands[2].type;
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
        p.deessOn = &deessOn;
        p.deessFreq = &deessFreq;
        p.deessThreshDb = &deessThreshDb;
        p.deessRangeDb = &deessRangeDb;
        for (size_t i = 0; i < bands.size(); ++i) {
            auto& b = p.bands[i];
            if (i >= 3) {
                b.on = &bands[i].on;
                b.freq = &bands[i].freq;
                b.gainDb = &bands[i].gainDb;
                b.q = &bands[i].q;
                b.type = &bands[i].type;
            }
            b.kind = &bands[i].kind;
            b.mode = &bands[i].mode;
            b.dynOn = &bands[i].dynOn;
            b.dynThreshDb = &bands[i].dynThreshDb;
            b.dynRangeDb = &bands[i].dynRangeDb;
            b.dynAttackMs = &bands[i].dynAttackMs;
            b.dynReleaseMs = &bands[i].dynReleaseMs;
        }
        p.tsOn = &tsOn;
        p.tsAttackDb = &tsAttackDb;
        p.tsSustainDb = &tsSustainDb;
        p.tsAttackMs = &tsAttackMs;
        p.tsReleaseMs = &tsReleaseMs;
        p.satOn = &satOn;
        p.satDriveDb = &satDriveDb;
        p.satWarmth = &satWarmth;
        p.satMix = &satMix;
        p.satTrimDb = &satTrimDb;
        p.bassMonoOn = &bassMonoOn;
        p.bassMonoFreq = &bassMonoFreq;
        p.widthOn = &widthOn;
        p.widthAmount = &widthAmount;
        return p;
    }
};

struct Rig {
    Params params;
    CopilotCorrectiveDsp dsp;
    void prepare()
    {
        dsp.bindParameters(params.bind());
        dsp.prepareToPlay(kRate, kBlock, 2);
    }
};

/// A stereo test signal: left / right sines at `hz`, `amp`; `rightSign`
/// -1 makes it pure side, +1 pure mid.
struct Measure {
    double inRms = 0.0, outRms = 0.0, outPeak = 0.0;
    bool finite = true;
    double gainDb() const { return 20.0 * std::log10(outRms / inRms); }
};

Measure runSine(CopilotCorrectiveDsp& dsp, double hz, float amp, float rightSign = 1.0f, double seconds = 0.6)
{
    Measure m;
    const int blocks = static_cast<int>(seconds * kRate / kBlock);
    juce::AudioBuffer<float> buffer(2, kBlock);
    long long n = 0;
    double inSq = 0.0, outSq = 0.0;
    long long counted = 0;
    for (int b = 0; b < blocks; ++b) {
        std::array<float, kBlock> input{};
        for (int i = 0; i < kBlock; ++i) {
            const auto v = static_cast<float>(amp * std::sin(2.0 * kPi * hz * static_cast<double>(n + i) / kRate));
            input[static_cast<size_t>(i)] = v;
            buffer.setSample(0, i, v);
            buffer.setSample(1, i, rightSign * v);
        }
        n += kBlock;
        dsp.process(buffer, false, kBlock);
        if (b < blocks / 2) continue;
        for (int i = 0; i < kBlock; ++i) {
            const double x = input[static_cast<size_t>(i)];
            for (int c = 0; c < 2; ++c) {
                const double y = buffer.getSample(c, i);
                m.finite = m.finite && std::isfinite(y);
                inSq += x * x;
                outSq += y * y;
                m.outPeak = std::max(m.outPeak, std::abs(y));
                ++counted;
            }
        }
    }
    m.inRms = std::sqrt(inSq / static_cast<double>(counted));
    m.outRms = std::sqrt(outSq / static_cast<double>(counted));
    return m;
}

float plain(KhensuCopilotProcessor& proc, const char* id) { return proc.getParamPlain(id); }

void setPlain(KhensuCopilotProcessor& proc, const char* id, float value)
{
    auto* p = proc.getAPVTS().getParameter(id);
    p->setValueNotifyingHost(p->convertTo0to1(value));
}

juce::MemoryBlock save(KhensuCopilotProcessor& proc)
{
    juce::MemoryBlock block;
    proc.getStateInformation(block);
    return block;
}

Recommendation rec(const std::string& id, std::map<std::string, float> params)
{
    Recommendation r;
    r.id = id;
    r.what = "Finding " + id;
    r.severity = 50.0f;
    r.safe_to_auto_apply = true;
    r.suggested_parameters = std::move(params);
    return r;
}
} // namespace

// ---- state v5 ------------------------------------------------------------------

TEST_CASE("A v4 state loads identically and the v5 parameters default to off (STU-10)",
          "[plugin][stu10][state][migration]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    KhensuCopilotProcessor source;
    source.setPlayConfigDetails(2, 2, kRate, kBlock);
    // A v4 session: bands 1-3 in use with their shapes, compressor, de-esser.
    setPlain(source, P::eqOn, 1.0f);
    setPlain(source, P::eqType, 1.0f);
    setPlain(source, P::eqFreq, 180.0f);
    setPlain(source, P::eqGainDb, -3.5f);
    setPlain(source, P::eq2On, 1.0f);
    setPlain(source, P::eq2Freq, 2600.0f);
    setPlain(source, P::eq2GainDb, 2.5f);
    setPlain(source, P::eq2Q, 2.2f);
    setPlain(source, P::eq3On, 1.0f);
    setPlain(source, P::eq3Type, 2.0f);
    setPlain(source, P::eq3Freq, 9000.0f);
    setPlain(source, P::eq3GainDb, 1.5f);
    setPlain(source, P::compOn, 1.0f);
    setPlain(source, P::deessOn, 1.0f);
    const auto block = save(source);
    std::unique_ptr<juce::XmlElement> xml(juce::AudioProcessor::getXmlFromBinary(block.getData(), (int)block.getSize()));
    REQUIRE(xml != nullptr);

    // Make it what a v4 build wrote: version 4, no v5 PARAM nodes and no v5
    // entries in the raw-value list.
    const auto v5ids = khensu::copilot_params::makeV5DspParamIds();
    std::set<juce::String> v5;
    for (const char* id : v5ids) v5.insert(id);
    REQUIRE(v5.size() == static_cast<size_t>(khensu::copilot_params::kNumV5DspParams));
    auto tree = juce::ValueTree::fromXml(*xml);
    tree.setProperty("khensuStateVersion", 4, nullptr);
    for (int i = tree.getNumChildren() - 1; i >= 0; --i)
        if (v5.count(tree.getChild(i).getProperty("id").toString()) != 0) tree.removeChild(i, nullptr);
    juce::StringArray raw;
    for (const auto& entry : juce::StringArray::fromTokens(tree.getProperty("khensuBoolRaw").toString(), ",", ""))
        if (v5.count(entry.upToFirstOccurrenceOf("=", false, false)) == 0) raw.add(entry);
    tree.setProperty("khensuBoolRaw", raw.joinIntoString(","), nullptr);
    std::unique_ptr<juce::XmlElement> v4xml(tree.createXml());
    juce::MemoryBlock v4Block;
    juce::AudioProcessor::copyXmlToBinary(*v4xml, v4Block);

    // Something else loaded first, so a stale v5 value would show.
    KhensuCopilotProcessor loaded;
    loaded.setPlayConfigDetails(2, 2, kRate, kBlock);
    setPlain(loaded, P::widthOn, 1.0f);
    setPlain(loaded, P::eq5On, 1.0f);
    setPlain(loaded, mc::kEqBandParamIds[0].kind, 2.0f);
    loaded.setStateInformation(v4Block.getData(), (int)v4Block.getSize());
    CHECK(loaded.lastStateRejection().isEmpty());
    for (const char* id : KhensuCopilotProcessor::kLegacyDspParamIds) {
        INFO(id);
        CHECK(plain(loaded, id) == Catch::Approx(plain(source, id)).margin(1e-4));
    }
    for (const char* id : v5ids) {
        INFO(id);
        auto* p = loaded.getAPVTS().getParameter(id);
        REQUIRE(p != nullptr);
        CHECK(p->getValue() == Catch::Approx(p->getDefaultValue()).margin(1e-6));
    }
    CHECK(plain(loaded, P::widthOn) < 0.5f);
    CHECK(plain(loaded, mc::kEqBandParamIds[0].kind) == 0.0f); // band 1 is its v3 shape again

    // The v4 chain sounds the same: the loaded instance and one set up by
    // hand with the same v1-v4 values give the same samples.
    KhensuCopilotProcessor byHand;
    byHand.setPlayConfigDetails(2, 2, kRate, kBlock);
    for (const char* id : KhensuCopilotProcessor::kLegacyDspParamIds) setPlain(byHand, id, plain(source, id));
    loaded.prepareToPlay(kRate, kBlock);
    byHand.prepareToPlay(kRate, kBlock);
    juce::Random rng(5);
    juce::MidiBuffer midi;
    double maxDiff = 0.0;
    for (int b = 0; b < 24; ++b) {
        juce::AudioBuffer<float> a(2, kBlock), c(2, kBlock);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < kBlock; ++i) {
                const float v = rng.nextFloat() * 0.6f - 0.3f;
                a.setSample(ch, i, v);
                c.setSample(ch, i, v);
            }
        loaded.processBlock(a, midi);
        byHand.processBlock(c, midi);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < kBlock; ++i)
                maxDiff = std::max(maxDiff, static_cast<double>(std::abs(a.getSample(ch, i) - c.getSample(ch, i))));
    }
    CHECK(maxDiff == 0.0);

    // It saves as v5 from then on, and migrateState stamps v5.
    const auto resaved = save(loaded);
    std::unique_ptr<juce::XmlElement> v5xml(juce::AudioProcessor::getXmlFromBinary(resaved.getData(), (int)resaved.getSize()));
    REQUIRE(v5xml != nullptr);
    CHECK(v5xml->getIntAttribute("khensuStateVersion") == 5);
    juce::ValueTree migrated("KhensuCopilotState");
    KhensuCopilotProcessor::migrateState(migrated, 4);
    CHECK(static_cast<int>(migrated.getProperty("khensuStateVersion")) == KhensuCopilotProcessor::kStateVersion);
}

TEST_CASE("A v4 EQ band runs the same filter it ran before (STU-10)", "[plugin][stu10][eq]")
{
    // The pre-STU-10 band: a JUCE ProcessorDuplicator'd IIR with the same
    // coefficients, run in the same 32-sample control blocks.
    using StereoIIR =
        juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>, juce::dsp::IIR::Coefficients<float>>;
    using AC = juce::dsp::IIR::ArrayCoefficients<float>;
    for (int shape : {0, 1, 2}) {
        INFO("shape " << shape);
        Rig rig;
        rig.params.bands[1].on = 1.0f;
        rig.params.bands[1].type = static_cast<float>(shape);
        rig.params.bands[1].freq = 1400.0f;
        rig.params.bands[1].gainDb = -4.5f;
        rig.params.bands[1].q = 1.7f;
        rig.prepare();

        StereoIIR ref;
        ref.state = new juce::dsp::IIR::Coefficients<float>(AC::makePeakFilter(kRate, 1000.0f, 1.0f, 1.0f));
        ref.prepare({kRate, static_cast<juce::uint32>(kBlock), 2});
        const float gain = juce::Decibels::decibelsToGain(-4.5f);
        *ref.state = shape == 1   ? AC::makeLowShelf(kRate, 1400.0f, 1.7f, gain)
                     : shape == 2 ? AC::makeHighShelf(kRate, 1400.0f, 1.7f, gain)
                                  : AC::makePeakFilter(kRate, 1400.0f, 1.7f, gain);
        ref.reset();

        juce::Random rng(11);
        double maxDiff = 0.0;
        // The chain's output gain and dry/wet ramp in over their first ~50 ms
        // after prepare (as in v4); compare once they have settled.
        constexpr int kSettleBlocks = 6;
        for (int b = 0; b < 16 + kSettleBlocks; ++b) {
            juce::AudioBuffer<float> a(2, kBlock), r(2, kBlock);
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < kBlock; ++i) {
                    const float v = rng.nextFloat() - 0.5f;
                    a.setSample(c, i, v);
                    r.setSample(c, i, v);
                }
            rig.dsp.process(a, false, kBlock);
            juce::dsp::AudioBlock<float> rb(r);
            for (int s = 0; s < kBlock; s += CopilotCorrectiveDsp::kControlBlockSamples) {
                auto sub = rb.getSubBlock(static_cast<size_t>(s), CopilotCorrectiveDsp::kControlBlockSamples);
                juce::dsp::ProcessContextReplacing<float> ctx(sub);
                ref.process(ctx);
            }
            if (b < kSettleBlocks) continue;
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < kBlock; ++i)
                    maxDiff = std::max(maxDiff, static_cast<double>(std::abs(a.getSample(c, i) - r.getSample(c, i))));
        }
        CHECK(maxDiff == 0.0);
    }
}

// ---- each new stage's response ---------------------------------------------------

TEST_CASE("EQ band kinds: notch, high-pass and low-pass (STU-10)", "[plugin][stu10][eq]")
{
    Rig rig;
    rig.params.bands[3].on = 1.0f; // band 4: notch at 1 kHz
    rig.params.bands[3].kind = 1.0f;
    rig.params.bands[3].freq = 1000.0f;
    rig.params.bands[3].q = 4.0f;
    rig.prepare();
    CHECK(runSine(rig.dsp, 1000.0, 0.25f).gainDb() < -30.0);
    CHECK(runSine(rig.dsp, 150.0, 0.25f).gainDb() == Catch::Approx(0.0).margin(0.3));

    rig.params.bands[3].on = 0.0f;
    rig.params.bands[4].on = 1.0f; // band 5: high-pass at 500 Hz
    rig.params.bands[4].kind = 2.0f;
    rig.params.bands[4].freq = 500.0f;
    rig.params.bands[4].q = 0.7071f;
    CHECK(runSine(rig.dsp, 100.0, 0.25f).gainDb() < -20.0);
    CHECK(runSine(rig.dsp, 6000.0, 0.25f).gainDb() == Catch::Approx(0.0).margin(0.3));

    rig.params.bands[4].on = 0.0f;
    rig.params.bands[5].on = 1.0f; // band 6: low-pass at 2 kHz
    rig.params.bands[5].kind = 3.0f;
    rig.params.bands[5].freq = 2000.0f;
    rig.params.bands[5].q = 0.7071f;
    CHECK(runSine(rig.dsp, 12000.0, 0.25f).gainDb() < -20.0);
    CHECK(runSine(rig.dsp, 200.0, 0.25f).gainDb() == Catch::Approx(0.0).margin(0.3));
}

TEST_CASE("An EQ band on mid or side leaves the other alone (STU-10)", "[plugin][stu10][eq]")
{
    Rig rig;
    rig.params.bands[0].on = 1.0f;
    rig.params.bands[0].type = 0.0f;
    rig.params.bands[0].freq = 1000.0f;
    rig.params.bands[0].gainDb = 6.0f;
    rig.params.bands[0].mode = 1.0f; // mid
    rig.prepare();
    CHECK(runSine(rig.dsp, 1000.0, 0.2f, 1.0f).gainDb() == Catch::Approx(6.0).margin(0.2));  // mono = mid
    CHECK(runSine(rig.dsp, 1000.0, 0.2f, -1.0f).gainDb() == Catch::Approx(0.0).margin(0.05)); // pure side
    rig.params.bands[0].mode = 2.0f; // side
    CHECK(runSine(rig.dsp, 1000.0, 0.2f, 1.0f).gainDb() == Catch::Approx(0.0).margin(0.05));
    CHECK(runSine(rig.dsp, 1000.0, 0.2f, -1.0f).gainDb() == Catch::Approx(6.0).margin(0.2));
}

TEST_CASE("A dynamic band cuts only while its band is over the threshold (STU-10)", "[plugin][stu10][dyneq]")
{
    Rig rig;
    auto& band = rig.params.bands[1];
    band.on = 1.0f;
    band.freq = 1000.0f;
    band.q = 1.0f;
    band.gainDb = 0.0f;
    band.dynOn = 1.0f;
    band.dynThreshDb = -30.0f;
    band.dynRangeDb = 6.0f;
    rig.prepare();
    // -12 dBFS at the band: 18 dB over, 4:1 asks 13.5 dB, the range caps at 6.
    const auto loud = runSine(rig.dsp, 1000.0, 0.25f);
    CHECK(loud.finite);
    CHECK(rig.dsp.bandReductionDbForTest(1) == Catch::Approx(6.0f).margin(0.1f));
    CHECK(loud.gainDb() == Catch::Approx(-6.0).margin(0.5));
    // Well under the threshold: untouched.
    const auto quiet = runSine(rig.dsp, 1000.0, 0.003f);
    CHECK(rig.dsp.bandReductionDbForTest(1) < 0.05f);
    CHECK(quiet.gainDb() == Catch::Approx(0.0).margin(0.1));
    // Out of the band: the detector does not see it.
    runSine(rig.dsp, 8000.0, 0.25f);
    CHECK(rig.dsp.bandReductionDbForTest(1) < 1.0f);
    // Dynamic off: the static band (0 dB) again.
    band.dynOn = 0.0f;
    CHECK(runSine(rig.dsp, 1000.0, 0.25f).gainDb() == Catch::Approx(0.0).margin(0.05));
}

TEST_CASE("Bass mono drops the low side and keeps the rest (STU-10)", "[plugin][stu10][bassmono]")
{
    Rig rig;
    rig.params.bassMonoOn = 1.0f;
    rig.params.bassMonoFreq = 150.0f;
    rig.prepare();
    CHECK(runSine(rig.dsp, 40.0, 0.3f, -1.0f, 1.2).gainDb() < -24.0);                         // low side: gone
    CHECK(runSine(rig.dsp, 4000.0, 0.3f, -1.0f).gainDb() == Catch::Approx(0.0).margin(0.1));  // high side kept
    CHECK(runSine(rig.dsp, 40.0, 0.3f, 1.0f, 1.2).gainDb() == Catch::Approx(0.0).margin(0.1)); // low mid kept (all-pass)
    CHECK(runSine(rig.dsp, 4000.0, 0.3f, 1.0f).gainDb() == Catch::Approx(0.0).margin(0.1));
}

TEST_CASE("Width scales the side and leaves the mid (STU-10)", "[plugin][stu10][width]")
{
    Rig rig;
    rig.params.widthOn = 1.0f;
    rig.params.widthAmount = 0.0f;
    rig.prepare();
    CHECK(runSine(rig.dsp, 500.0, 0.3f, -1.0f).outPeak < 1.0e-6);                           // side: gone
    CHECK(runSine(rig.dsp, 500.0, 0.3f, 1.0f).gainDb() == Catch::Approx(0.0).margin(0.01)); // mid: kept
    rig.params.widthAmount = 1.5f;
    CHECK(runSine(rig.dsp, 500.0, 0.3f, -1.0f).gainDb() == Catch::Approx(20.0 * std::log10(1.5)).margin(0.05));
}

TEST_CASE("Saturation soft-clips, and its mix and trim work (STU-10)", "[plugin][stu10][saturation]")
{
    Rig rig;
    rig.params.satOn = 1.0f;
    rig.params.satDriveDb = 20.0f;
    rig.params.satWarmth = 0.0f;
    rig.params.satMix = 1.0f;
    rig.params.satTrimDb = 0.0f;
    rig.prepare();
    // A 0.5 sine driven 10x: tanh holds the peaks just under 1.
    const auto hot = runSine(rig.dsp, 100.0, 0.5f);
    CHECK(hot.finite);
    CHECK(hot.outPeak < 1.05);
    CHECK(hot.outPeak > 0.9);
    // A quiet sine is in tanh's linear region: drive x gain.
    CHECK(runSine(rig.dsp, 100.0, 0.001f).gainDb() == Catch::Approx(20.0).margin(0.3));
    rig.params.satTrimDb = -20.0f;
    CHECK(runSine(rig.dsp, 100.0, 0.001f).gainDb() == Catch::Approx(0.0).margin(0.3));
    // Mix 0: the dry signal.
    rig.params.satMix = 0.0f;
    CHECK(runSine(rig.dsp, 100.0, 0.5f).gainDb() == Catch::Approx(0.0).margin(0.01));
    // Warmth adds even harmonics and a DC bias the blocker removes: stays finite and bounded.
    rig.params.satMix = 1.0f;
    rig.params.satTrimDb = 0.0f;
    rig.params.satWarmth = 1.0f;
    const auto warm = runSine(rig.dsp, 100.0, 0.5f);
    CHECK(warm.finite);
    CHECK(warm.outPeak < 2.2);
}

TEST_CASE("The transient shaper lifts attacks and leaves a steady tone (STU-10)", "[plugin][stu10][transient]")
{
    Rig rig;
    rig.params.tsOn = 1.0f;
    rig.params.tsAttackDb = 12.0f;
    rig.params.tsSustainDb = 0.0f;
    rig.prepare();
    // A steady tone: fast and slow followers agree (to the rectified ripple,
    // small at 2 kHz), gain = sustain = 0 dB.
    CHECK(runSine(rig.dsp, 2000.0, 0.2f, 1.0f, 1.2).gainDb() == Catch::Approx(0.0).margin(0.5));

    // A burst after silence: its first milliseconds are turned up.
    rig.dsp.reset();
    juce::AudioBuffer<float> buffer(2, kBlock);
    double onsetIn = 0.0, onsetOut = 0.0;
    for (int b = 0; b < 4; ++b) {
        for (int i = 0; i < kBlock; ++i) {
            const long long n = static_cast<long long>(b) * kBlock + i;
            const float v = n < 1024 ? 0.0f : static_cast<float>(0.2 * std::sin(2.0 * kPi * 300.0 * static_cast<double>(n) / kRate));
            buffer.setSample(0, i, v);
            buffer.setSample(1, i, v);
        }
        juce::AudioBuffer<float> in(buffer);
        rig.dsp.process(buffer, false, kBlock);
        for (int i = 0; i < kBlock; ++i) {
            const long long n = static_cast<long long>(b) * kBlock + i;
            if (n >= 1024 && n < 1024 + 240) { // the first 5 ms
                onsetIn += std::abs(in.getSample(0, i));
                onsetOut += std::abs(buffer.getSample(0, i));
            }
        }
    }
    CHECK(onsetOut > 1.5 * onsetIn);
}

TEST_CASE("The v5 stages stay finite under random automation and switching (STU-10)", "[plugin][stu10]")
{
    Rig rig;
    rig.prepare();
    juce::Random rng(91);
    juce::AudioBuffer<float> buffer(2, 256);
    bool finite = true;
    for (int it = 0; it < 400 && finite; ++it) {
        for (auto& b : rig.params.bands) {
            b.on = rng.nextBool() ? 1.0f : 0.0f;
            b.kind = static_cast<float>(rng.nextInt(4));
            b.type = static_cast<float>(rng.nextInt(3));
            b.mode = static_cast<float>(rng.nextInt(3));
            b.dynOn = rng.nextBool() ? 1.0f : 0.0f;
            b.freq = 60.0f + rng.nextFloat() * 15000.0f;
            b.gainDb = rng.nextFloat() * 24.0f - 12.0f;
            b.q = 0.3f + rng.nextFloat() * 7.7f;
            b.dynThreshDb = -60.0f * rng.nextFloat();
            b.dynRangeDb = 24.0f * rng.nextFloat();
            b.dynAttackMs = 0.1f + rng.nextFloat() * 199.9f;
            b.dynReleaseMs = 5.0f + rng.nextFloat() * 1995.0f;
        }
        rig.params.tsOn = rng.nextBool() ? 1.0f : 0.0f;
        rig.params.tsAttackDb = rng.nextFloat() * 48.0f - 24.0f;
        rig.params.tsSustainDb = rng.nextFloat() * 48.0f - 24.0f;
        rig.params.satOn = rng.nextBool() ? 1.0f : 0.0f;
        rig.params.satDriveDb = rng.nextFloat() * 24.0f;
        rig.params.satWarmth = rng.nextFloat();
        rig.params.bassMonoOn = rng.nextBool() ? 1.0f : 0.0f;
        rig.params.bassMonoFreq = 40.0f + rng.nextFloat() * 260.0f;
        rig.params.widthOn = rng.nextBool() ? 1.0f : 0.0f;
        rig.params.widthAmount = rng.nextFloat() * 2.0f;
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < 256; ++i) buffer.setSample(c, i, rng.nextFloat() * 2.0f - 1.0f);
        rig.dsp.process(buffer, false, 256);
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < 256; ++i) finite = finite && std::isfinite(buffer.getSample(c, i));
    }
    CHECK(finite);
}

// ---- real-time safety --------------------------------------------------------------

TEST_CASE("processBlock with every v5 stage on does not allocate (STU-10)", "[plugin][stu10][cq03]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    KhensuCopilotProcessor proc;
    proc.setPlayConfigDetails(2, 2, kRate, kBlock);
    for (int b = 0; b < mc::kNumEqBands; ++b) {
        const auto& ids = mc::kEqBandParamIds[b];
        setPlain(proc, ids.on, 1.0f);
        setPlain(proc, ids.kind, static_cast<float>(b % 4));
        setPlain(proc, ids.mode, static_cast<float>(b % 3));
        setPlain(proc, ids.dynOn, b % 2 == 0 ? 1.0f : 0.0f);
        setPlain(proc, ids.dynThreshDb, -40.0f);
    }
    for (const char* on : {P::tsOn, P::satOn, P::bassMonoOn, P::widthOn, P::compOn, P::deessOn, P::hpfOn, P::limOn})
        setPlain(proc, on, 1.0f);
    setPlain(proc, P::widthAmount, 1.3f);
    proc.prepareToPlay(kRate, kBlock);

    juce::AudioBuffer<float> buffer(2, kBlock);
    juce::MidiBuffer midi;
    juce::Random rng(3);
    const auto fill = [&] {
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < kBlock; ++i) buffer.setSample(c, i, rng.nextFloat() * 0.8f - 0.4f);
    };
    const auto automate = [&](const char* id, float value) {
        auto* p = proc.getAPVTS().getParameter(id);
        p->setValue(p->convertTo0to1(value));
    };
    for (int b = 0; b < 4; ++b) {
        fill();
        proc.processBlock(buffer, midi);
    }
    khensu::test::resetViolations();
    for (int b = 0; b < 32; ++b) {
        fill();
        khensu::test::ScopedAllocationGuard guard;
        // Host automation inside the callback: band freqs, kinds, the stages.
        automate(mc::kEqBandParamIds[3].freq, 500.0f + static_cast<float>((b % 7) * 600));
        automate(mc::kEqBandParamIds[1].kind, static_cast<float>(b % 4));
        automate(mc::kEqBandParamIds[2].mode, static_cast<float>(b % 3));
        automate(mc::kEqBandParamIds[4].dynOn, b % 2 == 0 ? 1.0f : 0.0f);
        automate(P::satDriveDb, static_cast<float>(b % 12));
        automate(P::widthAmount, 0.5f + static_cast<float>(b % 10) * 0.1f);
        automate(P::bassMonoFreq, 60.0f + static_cast<float>((b % 5) * 40));
        automate(P::tsOn, b % 3 == 0 ? 0.0f : 1.0f);
        proc.processBlock(buffer, midi);
    }
    CHECK(khensu::test::violationCount() == 0);
}

// ---- Apply all safe fixes -----------------------------------------------------------

TEST_CASE("Apply all maps width, bass mono, tonal + resonance and masking fixes (STU-10)",
          "[plugin][stu10][applyall]")
{
    Recommendation tooWide = rec("stereo_too_wide_f", {});
    khensu::EvidenceItem e;
    e.measurement = "Stereo width";
    e.value = 0.8f;
    e.threshold = 0.6f;
    e.comparison = "above";
    tooWide.evidence.push_back(e);
    const std::vector<Recommendation> recs = {
        rec("tilt_too_bright_f", {{"eq_type", 1.0f}, {"eq_freq_hz", 1000.0f}, {"eq_gain_db", -2.0f}, {"eq_q", 0.707f}}),
        rec("low_end_heavy_f", {{"eq_type", 2.0f}, {"eq_freq_hz", 120.0f}, {"eq_gain_db", -1.5f}, {"eq_q", 0.707f}}),
        rec("centroid_too_bright_f", {{"eq_type", 1.0f}, {"eq_freq_hz", 6000.0f}, {"eq_gain_db", -1.0f}}),
        rec("resonance_hotspot_2500hz_f", {{"eq_center_hz", 2500.0f}, {"eq_q", 6.0f}, {"eq_cut_db", 4.0f}}),
        rec("masking_collision_vocals_other_f", {{"eq_freq_hz", 2000.0f},
                                                 {"eq_gain_db", -3.0f},
                                                 {"dyneq_freq_hz", 2000.0f},
                                                 {"dyneq_q", 1.4f},
                                                 {"dyneq_threshold_db", -30.0f},
                                                 {"dyneq_max_cut_db", 3.0f},
                                                 {"dyneq_key_stem", 0.0f},
                                                 {"dyneq_target_stem", 3.0f}}),
        rec("sub_bass_too_wide_f", {}),
        tooWide,
        rec("transient_f", {{"transient_attack_db", 3.0f}}),
        rec("saturation_f", {{"saturation_drive_db", 4.0f}}),
    };
    const auto none = [](const std::string&) { return 0.0f; };
    const auto batch = khensu::copilot::combineFixes(recs, 1.0f, none, true);
    for (const auto& s : batch.skipped) UNSCOPED_INFO("skipped " << s.id << ": " << s.reason);
    CHECK(batch.skipped.empty());
    CHECK(batch.appliedIds.size() == recs.size());
    // Five EQ moves on five bands, in order; the masking cut is dynamic.
    CHECK(batch.values.at("eq_type") == static_cast<float>(mc::EqBandShape::HighShelf));
    CHECK(batch.values.at("eq2_type") == static_cast<float>(mc::EqBandShape::LowShelf));
    CHECK(batch.values.at("eq3_freq") == Catch::Approx(6000.0f));
    CHECK(batch.values.at("eq4_gain_db") == Catch::Approx(-4.0f));
    CHECK(batch.values.at("eq5_dyn_on") == 1.0f);
    CHECK(batch.values.at("eq5_dyn_range_db") == Catch::Approx(3.0f));
    CHECK(batch.values.count("eq6_on") == 0);
    CHECK(batch.values.at("bassmono_on") == 1.0f);
    CHECK(batch.values.at("width_on") == 1.0f);
    CHECK(batch.values.at("width_amount") < 1.0f);
    CHECK(batch.values.at("ts_attack_db") == Catch::Approx(3.0f));
    CHECK(batch.values.at("sat_drive_db") == Catch::Approx(4.0f));

    // In the processor: applied as one step, the stages switched on.
    juce::ScopedJuceInitialiser_GUI juceInit;
    KhensuCopilotProcessor proc;
    proc.setPlayConfigDetails(2, 2, kRate, kBlock);
    const auto result = proc.applyAllSafeFixes(recs);
    REQUIRE(result.applied);
    CHECK(plain(proc, P::eq5On) > 0.5f);
    CHECK(plain(proc, mc::kEqBandParamIds[4].dynOn) > 0.5f);
    CHECK(plain(proc, P::bassMonoOn) > 0.5f);
    CHECK(plain(proc, P::widthOn) > 0.5f);
    CHECK(proc.isAiSet(P::widthAmount));
    proc.undoHistory().undo();
    CHECK(plain(proc, P::widthOn) < 0.5f);
    CHECK(plain(proc, P::eq5On) < 0.5f);
}

TEST_CASE("What Apply all still skips, and why (STU-10)", "[plugin][stu10][applyall]")
{
    const auto none = [](const std::string&) { return 0.0f; };
    const auto batch = khensu::copilot::combineFixes(
        {rec("noise_floor_high_f", {{"denoise_reduction_db", 6.0f}}), rec("phase_issue_f", {{"correlation_target", 0.0f}}),
         rec("reverb_f", {{"reverb_wet", 0.2f}}), rec("ms_brightness_imbalance_f", {})},
        1.0f, none, true);
    CHECK(batch.appliedIds.empty());
    REQUIRE(batch.skipped.size() == 4);
    CHECK(batch.skipped[0].reason.contains("denoise_reduction_db"));
    CHECK(batch.skipped[1].reason.contains("correlation_target"));
    CHECK(batch.skipped[3].reason.contains("no parameter targets"));
}

// ---- editor ------------------------------------------------------------------------

TEST_CASE("The More stages page covers the v5 parameters and rebinds to the chosen band (STU-10)",
          "[plugin][stu10][ui]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    KhensuCopilotProcessor proc;
    proc.setPlayConfigDetails(2, 2, kRate, kBlock);
    khensu::DspControlGridPanel more(proc, khensu::DspControlGridPanel::Page::More);
    // EQ 4-6 (3 knobs each), band dynamics (4), transient (4), saturation (4),
    // bass mono (1), width (1); switches: 3 bands, dynamic, 4 stages; shapes:
    // 3 band shapes + the band's filter and channels.
    CHECK(more.getNumKnobsForTest() == 23);
    CHECK(more.getNumTogglesForTest() == 8);
    CHECK(more.getNumShapeSelectorsForTest() == 5);
    CHECK(more.knobForTest(mc::kEqBandParamIds[0].dynThreshDb) != nullptr);
    more.selectBand(4);
    CHECK(more.selectedBand() == 4);
    CHECK(more.knobForTest(mc::kEqBandParamIds[0].dynThreshDb) == nullptr);
    auto* thresh = more.knobForTest(mc::kEqBandParamIds[3].dynThreshDb);
    REQUIRE(thresh != nullptr);
    CHECK(thresh->getTitle().contains("EQ4"));
    // The rebound knob drives band 4's parameter.
    thresh->setValue(-42.0, juce::sendNotificationSync);
    CHECK(plain(proc, mc::kEqBandParamIds[3].dynThreshDb) == Catch::Approx(-42.0f).margin(0.1f));
    CHECK(plain(proc, mc::kEqBandParamIds[0].dynThreshDb) == Catch::Approx(-30.0f).margin(0.1f));

    // Every knob is inside the grid with room for its value, at both sizes.
    for (auto [w, h] : {std::pair{900, 392}, std::pair{1230, 610}}) {
        more.setSize(w, h);
        for (auto* child : more.getChildren())
            if (auto* knob = dynamic_cast<khensu::ui::KhensuKnob*>(child)) {
                INFO(knob->getTitle() << " at " << w << "x" << h);
                CHECK(more.getLocalBounds().contains(knob->getBounds()));
                CHECK_FALSE(knob->getBounds().isEmpty());
            }
    }
}
