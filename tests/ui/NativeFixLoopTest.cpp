// tests/ui/NativeFixLoopTest.cpp
// OVH-9: the core loop without the Python service. A deterministic mix with
// a +6 dB low-shelf defect is analysed by the native pass only (the deep
// inputs marked failed, as AnalysisController does when the engine does not
// answer), the low-end finding's own fix chain (buildRecommendationChain) is
// rendered natively, the render is graded again, and the fix check
// (FixVerification) reports the finding resolved - while every finding built
// on an engine input reads "could not check", never "resolved".
#include <catch2/catch_test_macros.hpp>
#include "analysis/FixVerification.h"
#include "session/RecommendationChain.h"
#include "session/RenderService.h"
#include "support/CleanTestMix.h"
#include <khensu/analysis/AnalysisInputs.h>
#include <khensu/analysis/AudioAnalyzer.h>
#include <khensu/dsp/ChainProcessor.h>
#include <khensu/dsp/OfflineChainRenderer.h>
#include <khensu/mixerchain/RecommendationDspMapper.h>
#include <khensu/recommendations/MidSideImbalanceRecommender.h>
#include <khensu/recommendations/RecommendationEngine.h>
#include <khensu/targets/CreativeTarget.h>
#include <khensu/targets/GenreDatabase.h>
#include <JuceHeader.h>

#include <cmath>

using namespace khensu;
namespace mix = khensu::test::clean_mix;

namespace {

juce::File writeMix(const mix::Stereo& y, const juce::String& name)
{
    auto file = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile(name);
    file.deleteFile();
    juce::WavAudioFormat fmt;
    std::unique_ptr<juce::OutputStream> stream = file.createOutputStream();
    REQUIRE(stream != nullptr);
    const auto options = juce::AudioFormatWriterOptions{}
                             .withSampleRate(static_cast<double>(mix::kRate))
                             .withNumChannels(2)
                             .withBitsPerSample(32)
                             .withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);
    auto writer = fmt.createWriterFor(stream, options);
    REQUIRE(writer != nullptr);
    const float* chans[2] = {y[0].data(), y[1].data()};
    REQUIRE(writer->writeFromFloatArrays(chans, 2, static_cast<int>(y[0].size())));
    return file;
}

/// The genre the mix is graded against, from the shipped genre targets:
/// garage, whose low-end ceiling (share 0.53) the +6 dB shelf clearly
/// crosses (pop's, 0.81, it does not).
GenreEntry gradingGenre()
{
    GenreDatabase genres;
    const auto targets = juce::File(juce::String(__FILE__))
                             .getParentDirectory()
                             .getParentDirectory()
                             .getParentDirectory()
                             .getChildFile("resources/genre_norms/genre_targets.json")
                             .getFullPathName();
    REQUIRE(genres.loadFromFile(targets.toStdString()).is_ok());
    const auto genre = genres.findById("garage");
    REQUIRE(genre.has_value());
    return *genre;
}

/// The native pass the way the app runs it with the engine down.
AnalysisResult nativePass(const juce::File& file)
{
    AudioFileRef ref;
    ref.id = "mix"; // the same file id on both sides: the same issue codes
    ref.path = file.getFullPathName().toStdString();
    ref.format = "wav";
    auto analysed = AudioAnalyzer{}.analyze(ref, "ovh9");
    REQUIRE(analysed.is_ok());
    auto r = analysed.value();
    r.file_id = ref.id;
    r.inputs_failed |= analysis_inputs::serviceInputs(false);
    r.recommendations = RecommendationEngine{}.generate(r, gradingGenre(), CreativeTarget{});
    return r;
}

} // namespace

TEST_CASE("OVH-9: a low-end defect is fixed and checked without the service", "[ovh9][render][e2e]")
{
    auto y = mix::cleanMix();
    mix::shelf(y, false, 120.0, 6.0);
    const auto defect = writeMix(y, "khensu_ovh9_defect.wav");
    const auto before = nativePass(defect);

    const Recommendation* lowEnd = nullptr;
    std::string codes;
    for (const auto& rec : before.recommendations)
        codes += issueCodeOf(rec, before.file_id) + "(" + std::to_string(static_cast<int>(rec.severity)) + ") ";
    INFO("native findings: " << codes << " balance_low " << before.spectral.balance_low);
    for (const auto& rec : before.recommendations)
        if (rec.type != RecommendationType::Strength && issueCodeOf(rec, before.file_id) == "low_end_heavy")
            lowEnd = &rec;
    REQUIRE(lowEnd != nullptr);

    const auto chain = buildRecommendationChain(*lowEnd);
    REQUIRE_FALSE(chain.empty());
    // With the engine down this is exactly the chain RenderService renders natively.
    REQUIRE(RenderService::shouldRenderNatively(false, chain));
    REQUIRE_FALSE(RenderService::shouldRenderNatively(true, chain));

    const auto fixed = defect.getSiblingFile("khensu_ovh9_defect_fix.wav");
    const auto rendered = dsp::OfflineChainRenderer::renderFile(defect, fixed, RenderService::nativeChain(chain));
    INFO(rendered.error);
    REQUIRE(rendered.status == dsp::OfflineRenderResult::Status::Rendered);
    CHECK(rendered.guardrailOk);

    const auto after = nativePass(fixed);
    const auto check = fix_verification::compare(before, after, fixed.getFullPathName().toStdString());
    INFO(fix_verification::summary(check));
    const auto* finding = check.find("low_end_heavy");
    REQUIRE(finding != nullptr);
    CHECK(finding->outcome == fix_verification::Outcome::Resolved);
    // An engine-built finding is never claimed fixed by a native-only check.
    for (const auto& f : check.findings)
        if (f.outcome == fix_verification::Outcome::Resolved) {
            const auto it = std::find_if(before.recommendations.begin(), before.recommendations.end(),
                                         [&](const Recommendation& r) { return issueCodeOf(r, before.file_id) == f.code; });
            REQUIRE(it != before.recommendations.end());
            CHECK(fix_verification::couldMeasure(after, *it));
        }
    defect.deleteFile();
    fixed.deleteFile();
}

TEST_CASE("FN2-2a: a hiss defect is fixed natively and checked without the service", "[ovh9][fn2-2a][render][e2e]")
{
    // The clean mix with two near-silent passages (where a noise floor is
    // read) and a steady hiss about 46 dB down under everything: the
    // noise-floor finding, whose fix (SpectralDenoise) used to need the engine.
    auto y = mix::cleanMix();
    std::mt19937 rng(424242);
    std::uniform_real_distribution<float> uni(-1.0f, 1.0f);
    const float hiss = std::pow(10.0f, -46.0f / 20.0f) * std::sqrt(3.0f); // uniform: RMS = amp / sqrt(3)
    const auto quiet = [](int i) {
        const double t = static_cast<double>(i) / mix::kRate;
        return (t >= 6.0 && t < 7.5) || (t >= 13.0 && t < 14.5);
    };
    for (int i = 0; i < mix::kFrames; ++i)
        for (auto& ch : y) {
            auto& s = ch[static_cast<size_t>(i)];
            s = (quiet(i) ? 0.0f : s) + hiss * uni(rng);
        }
    const auto defect = writeMix(y, "khensu_fn22a_hiss.wav");
    const auto before = nativePass(defect);
    INFO("noise floor " << before.artifacts.noise_floor_db << " dBFS");
    const Recommendation* noise = nullptr;
    for (const auto& rec : before.recommendations)
        if (rec.type != RecommendationType::Strength && issueCodeOf(rec, before.file_id) == "noise_floor_high")
            noise = &rec;
    REQUIRE(noise != nullptr);

    const auto chain = buildRecommendationChain(*noise);
    REQUIRE(chain.size() == 1);
    CHECK(chain.front().effect == "SpectralDenoise");
    // Engine down: the denoise renders natively now.
    REQUIRE(RenderService::shouldRenderNatively(false, chain));

    const auto fixed = defect.getSiblingFile("khensu_fn22a_hiss_fix.wav");
    const auto rendered = dsp::OfflineChainRenderer::renderFile(defect, fixed, RenderService::nativeChain(chain));
    INFO(rendered.error);
    REQUIRE(rendered.status == dsp::OfflineRenderResult::Status::Rendered);
    CHECK(rendered.guardrailOk);

    const auto after = nativePass(fixed);
    INFO("noise floor after " << after.artifacts.noise_floor_db << " dBFS");
    const auto check = fix_verification::compare(before, after, fixed.getFullPathName().toStdString());
    INFO(fix_verification::summary(check));
    const auto* finding = check.find("noise_floor_high");
    REQUIRE(finding != nullptr);
    CHECK(finding->outcome == fix_verification::Outcome::Resolved);
    CHECK(after.artifacts.noise_floor_db < before.artifacts.noise_floor_db - 4.0f);
    defect.deleteFile();
    fixed.deleteFile();
}

// STU-18: the sub-too-wide finding carries bass_mono_hz, its chain is a
// BassMonoMaker at that crossover, and the native render of that chain
// brings the measured ratio (side / mid energy, 20-100 Hz, as
// extended_analyzer.py measures it) far under the finding's 0.70 gate.
TEST_CASE("A sub too wide renders a bass mono that clears it (STU-18)", "[stu18][fixloop]")
{
    AnalysisResult r;
    r.file_id = "f_sub";
    r.mid_side_imbalance = AnalysisResult::MidSideImbalanceData{};
    r.mid_side_imbalance->side_heavy_ratio = 0.95f;
    const auto recs = MidSideImbalanceRecommender{}.recommend(r, {}, {});
    REQUIRE(recs.size() == 1);
    REQUIRE(recs[0].suggested_parameters.count("bass_mono_hz") == 1);
    CHECK(juce::exactlyEqual(recs[0].suggested_parameters.at("bass_mono_hz"), mixerchain::kSubBassMonoHz)); // the plugin's too
    const auto chain = buildRecommendationChain(recs[0]);
    REQUIRE(chain.size() == 1);
    CHECK(chain[0].effect == "BassMonoMaker");
    CHECK(juce::exactlyEqual(chain[0].parameters.at("crossover_hz"), MidSideImbalanceRecommender::kBassMonoHz));
    CHECK(RenderService::shouldRenderNatively(false, chain));

    // A sub at full width: side tones as loud as the mid's, 90 degrees apart
    // (uncorrelated), plus a wide 1 kHz the fix must leave wide.
    constexpr double sr = 48000.0, pi = 3.14159265358979323846;
    const int n = static_cast<int>(sr * 2.0);
    const double subs[] = {40.0, 60.0, 90.0};
    dsp::PlanarBlock in;
    in.ch.assign(2, std::vector<double>(static_cast<size_t>(n), 0.0));
    for (int i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / sr;
        double mid = 0.0, side = 0.2 * std::cos(2.0 * pi * 1000.0 * t);
        for (const double hz : subs) {
            mid += 0.15 * std::sin(2.0 * pi * hz * t);
            side += 0.15 * std::cos(2.0 * pi * hz * t);
        }
        in.ch[0][static_cast<size_t>(i)] = mid + side;
        in.ch[1][static_cast<size_t>(i)] = mid - side;
    }
    dsp::ChainProcessor fix;
    std::string error;
    REQUIRE(fix.prepare(RenderService::nativeChain(chain), sr, 2, error));
    dsp::PlanarBlock out;
    out.ch.resize(2);
    const auto sink = [&](const dsp::PlanarBlock& b) {
        for (size_t c = 0; c < 2; ++c) out.ch[c].insert(out.ch[c].end(), b.ch[c].begin(), b.ch[c].end());
    };
    fix.process(in, sink);
    fix.finish(sink);
    REQUIRE(out.numSamples() == n);

    // Energy of the given tones in mid and side over the last second.
    const auto tonePower = [&](const dsp::PlanarBlock& y, bool sideOf, double hz) {
        const int from = n - static_cast<int>(sr);
        double re = 0.0, im = 0.0;
        for (int i = from; i < n; ++i) {
            const auto k = static_cast<size_t>(i);
            const double v = 0.5 * (y.ch[0][k] + (sideOf ? -y.ch[1][k] : y.ch[1][k]));
            const double ph = 2.0 * pi * hz * static_cast<double>(i - from) / sr;
            re += v * std::cos(ph);
            im -= v * std::sin(ph);
        }
        return re * re + im * im;
    };
    const auto ratio = [&](const dsp::PlanarBlock& y) {
        double side = 0.0, mid = 0.0;
        for (const double hz : subs) {
            side += tonePower(y, true, hz);
            mid += tonePower(y, false, hz);
        }
        return side / mid;
    };
    INFO("ratio before " << ratio(in) << ", after " << ratio(out));
    CHECK(ratio(in) > MidSideImbalanceRecommender::kSideHeavyRatio);
    CHECK(ratio(out) < 0.1);
    // The mid's sub and the 1 kHz image are kept.
    CHECK(std::abs(10.0 * std::log10(tonePower(out, false, 60.0) / tonePower(in, false, 60.0))) < 0.5);
    CHECK(std::abs(10.0 * std::log10(tonePower(out, true, 1000.0) / tonePower(in, true, 1000.0))) < 0.5);
}
