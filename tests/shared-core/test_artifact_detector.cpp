#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <khensu/analysis/ArtifactDetector.h>

#include <algorithm>
#include <cmath>
#include <vector>

using namespace khensu;

static constexpr double kPi = 3.14159265358979323846;

static void generateSine(std::vector<float>& left, std::vector<float>& right, double freq, double amplitude,
                         double sampleRate, int numSamples)
{
    left.resize(numSamples);
    right.resize(numSamples);
    for (int i = 0; i < numSamples; ++i) {
        const float s = static_cast<float>(amplitude * std::sin(2.0 * kPi * freq * i / sampleRate));
        left[i] = s;
        right[i] = s;
    }
}

// Alternates +amplitude/-amplitude every single sample (Nyquist-rate square
// wave) - every sample is a direction reversal, which is exactly the
// high-frequency-chatter signature the AI-artifact heuristic looks for.
static void generateNyquistAlternating(std::vector<float>& left, std::vector<float>& right, float amplitude,
                                       int numSamples)
{
    left.resize(numSamples);
    right.resize(numSamples);
    for (int i = 0; i < numSamples; ++i) {
        const float s = (i % 2 == 0) ? amplitude : -amplitude;
        left[i] = s;
        right[i] = s;
    }
}

TEST_CASE("ArtifactDetector: fresh instance with no samples returns default result", "[artifacts]")
{
    ArtifactDetector ad;
    ad.reset(48000.0);

    auto result = ad.getResult();
    CHECK_FALSE(result.has_clipping);
    CHECK(result.clipping_frame_count == 0);
    CHECK(result.noise_floor_db == -90.0f);
}

TEST_CASE("ArtifactDetector: samples at full scale are flagged as clipping", "[artifacts]")
{
    const double fs = 48000.0;
    const int dur = static_cast<int>(fs * 1.0);

    ArtifactDetector ad;
    ad.reset(fs);

    // Full-scale square wave: every sample sits at |1.0|, at/above kClipThreshold.
    std::vector<float> L, R;
    generateNyquistAlternating(L, R, 1.0f, dur);
    const float* ptrs[] = {L.data(), R.data()};
    ad.process(ptrs, 2, dur);

    auto result = ad.getResult();
    CHECK(result.has_clipping);
    CHECK(result.clipping_frame_count == dur);
}

TEST_CASE("ArtifactDetector: sub-clipping sine reports no clipping", "[artifacts]")
{
    const double fs = 48000.0;
    const int dur = static_cast<int>(fs * 2.0);

    ArtifactDetector ad;
    ad.reset(fs);

    std::vector<float> L, R;
    generateSine(L, R, 1000.0, 0.5, fs, dur);
    const float* ptrs[] = {L.data(), R.data()};
    ad.process(ptrs, 2, dur);

    auto result = ad.getResult();
    CHECK_FALSE(result.has_clipping);
    CHECK(result.clipping_frame_count == 0);
}

TEST_CASE("ArtifactDetector: noise floor matches a constant-level quiet signal", "[artifacts]")
{
    // A steady -43 dBFS-RMS sine has no dynamic range, so every 100ms block's
    // RMS - and therefore the 5th-percentile "noise floor" - should land on
    // the same value: 20*log10(amplitude / sqrt(2)).
    const double fs = 48000.0;
    const int dur = static_cast<int>(fs * 3.0);
    const double amplitude = 0.01;
    const float expectedDb = static_cast<float>(20.0 * std::log10(amplitude / std::sqrt(2.0)));

    ArtifactDetector ad;
    ad.reset(fs);

    std::vector<float> L, R;
    generateSine(L, R, 1000.0, amplitude, fs, dur);
    const float* ptrs[] = {L.data(), R.data()};
    ad.process(ptrs, 2, dur);

    auto result = ad.getResult();
    CHECK_THAT(result.noise_floor_db, Catch::Matchers::WithinAbs(expectedDb, 1.0f));
}

TEST_CASE("ArtifactDetector: sample-to-sample chatter is reported as a raw rate, never as AI", "[artifacts][audit]")
{
    // Every sample reversing direction gives a reversal rate near 1.0. The
    // fast pass has no calibrated AI detector (AUDIT-010), so even this
    // extreme input sets no AI flag or confidence - only the raw rate.
    const double fs = 48000.0;
    const int dur = static_cast<int>(fs * 1.0);

    ArtifactDetector ad;
    ad.reset(fs);

    std::vector<float> L, R;
    generateNyquistAlternating(L, R, 0.5f, dur);
    const float* ptrs[] = {L.data(), R.data()};
    ad.process(ptrs, 2, dur);

    auto result = ad.getResult();
    CHECK(result.sample_reversal_rate > 0.99f);
}

TEST_CASE("ArtifactDetector: smooth low-frequency sine has a low reversal rate", "[artifacts]")
{
    // A 100 Hz sine reverses direction only twice per cycle - rate ~2*f/fs.
    const double fs = 48000.0;
    const int dur = static_cast<int>(fs * 2.0);

    ArtifactDetector ad;
    ad.reset(fs);

    std::vector<float> L, R;
    generateSine(L, R, 100.0, 0.5, fs, dur);
    const float* ptrs[] = {L.data(), R.data()};
    ad.process(ptrs, 2, dur);

    auto result = ad.getResult();
    CHECK_THAT(result.sample_reversal_rate, Catch::Matchers::WithinAbs(2.0f * 100.0f / 48000.0f, 1e-3f));
}

TEST_CASE("ArtifactDetector: reset() clears prior state", "[artifacts]")
{
    const double fs = 48000.0;
    const int dur = static_cast<int>(fs * 1.0);

    ArtifactDetector ad;
    ad.reset(fs);

    std::vector<float> L, R;
    generateNyquistAlternating(L, R, 1.0f, dur);
    {
        const float* p[] = {L.data(), R.data()};
        ad.process(p, 2, dur);
    }

    REQUIRE(ad.getResult().has_clipping);

    ad.reset(fs);
    auto result = ad.getResult();
    CHECK_FALSE(result.has_clipping);
    CHECK(result.clipping_frame_count == 0);
    CHECK(result.noise_floor_db == -90.0f);
}

TEST_CASE("ArtifactDetector: an isolated full-scale sample is not clipping", "[artifacts][audit]")
{
    // A 16-bit -32768 sample decodes to exactly -1.0 - legal, not a clip.
    const double fs = 48000.0;
    const int dur = static_cast<int>(fs);
    std::vector<float> L, R;
    generateSine(L, R, 1000.0, 0.5, fs, dur);
    L[1000] = -1.0f;
    L[5000] = 1.0f;
    L[5001] = 1.0f; // a 2-sample run is still below kMinClipRun
    ArtifactDetector ad;
    ad.reset(fs);
    const float* ptrs[] = {L.data(), R.data()};
    ad.process(ptrs, 2, dur);
    CHECK_FALSE(ad.getResult().has_clipping);

    // Three consecutive full-scale samples on one channel are a clip.
    L[9000] = L[9001] = L[9002] = 1.0f;
    ArtifactDetector ad2;
    ad2.reset(fs);
    ad2.process(ptrs, 2, dur);
    CHECK(ad2.getResult().has_clipping);
    CHECK(ad2.getResult().clipping_frame_count == 3);
}

TEST_CASE("ArtifactDetector: a dense loud master has no measurable noise floor", "[artifacts][audit]")
{
    // Loud programme with no quiet passages: its quietest blocks are music,
    // not noise, so no floor is reported (-90) instead of ~-15 dBFS.
    const double fs = 48000.0;
    const int dur = static_cast<int>(fs * 4.0);
    std::vector<float> L, R;
    generateSine(L, R, 220.0, 0.5, fs, dur);
    for (int i = 0; i < dur; ++i) {
        const float env = 0.6f + 0.4f * std::sin(static_cast<float>(i) * 1.0e-4f); // gentle level movement
        L[static_cast<size_t>(i)] *= env;
        R[static_cast<size_t>(i)] *= env;
    }
    ArtifactDetector ad;
    ad.reset(fs);
    const float* ptrs[] = {L.data(), R.data()};
    ad.process(ptrs, 2, dur);
    CHECK(ad.getResult().noise_floor_db == -90.0f);
}

TEST_CASE("ArtifactDetector: noise floor is measured from the quiet passages", "[artifacts][audit]")
{
    // 2 s of loud music, then 1 s of -60 dBFS-RMS hiss-like tone.
    const double fs = 48000.0;
    const int loud = static_cast<int>(fs * 2.0), quiet = static_cast<int>(fs * 1.0);
    std::vector<float> L, R, qL, qR;
    generateSine(L, R, 440.0, 0.5, fs, loud);
    const double qAmp = std::pow(10.0, -60.0 / 20.0) * std::sqrt(2.0);
    generateSine(qL, qR, 3000.0, qAmp, fs, quiet);
    L.insert(L.end(), qL.begin(), qL.end());
    R.insert(R.end(), qR.begin(), qR.end());
    ArtifactDetector ad;
    ad.reset(fs);
    const float* ptrs[] = {L.data(), R.data()};
    ad.process(ptrs, 2, loud + quiet);
    CHECK(std::abs(ad.getResult().noise_floor_db - (-60.0f)) < 1.0f);
}

#include "TestNoise.h"
#include <khensu/recommendations/ArtifactRecommender.h>
#include <khensu/recommendations/MixScorecard.h>

TEST_CASE("ArtifactDetector: white/pink/brown noise get no AI confidence and no penalty", "[artifacts][audit]")
{
    // AUDIT-010: (rate - 0.40) * 10 read white 0.667, pink 0.584 and brown
    // 0.447 all as "100% AI", and the scorecard took up to 100 points off.
    struct Case {
        const char* name;
        double exponent;
        float expectedRate;
    };
    for (const auto& c : {Case{"white", 0.0, 0.667f}, Case{"pink", 1.0, 0.584f}, Case{"brown", 2.0, 0.447f}}) {
        INFO(c.name);
        const auto noise = khensu::test::colouredNoise(18, c.exponent, 7u, 0.1);
        ArtifactDetector ad;
        ad.reset(48000.0);
        const float* ptrs[] = {noise.data(), noise.data()};
        ad.process(ptrs, 2, static_cast<int>(noise.size()));
        const auto art = ad.getResult();
        CHECK_THAT(art.sample_reversal_rate, Catch::Matchers::WithinAbs(c.expectedRate, 0.03f));

        AnalysisResult r;
        r.file_id = "noise";
        r.artifacts = art;
        r.harshness = AnalysisResult::HarshnessData{}; // FX & Processing available, harshness 0
        const auto recs = ArtifactRecommender{}.recommend(r, GenreEntry{}, CreativeTarget{});
        CHECK(std::none_of(recs.begin(), recs.end(),
                           [](const Recommendation& x) { return x.id.rfind("ai_artifacts_", 0) == 0; }));

        const auto sc = buildMixScorecard(r, GenreEntry{}, recs, ScoreWeightProfile{});
        for (const auto& cat : sc.categories) {
            if (cat.id == "fx_processing") CHECK(cat.score == 100.0f);
            if (cat.id == "technical_integrity")
                for (const auto& sub : cat.sub_categories)
                    if (sub.id == "noise_artefacts") // the noise floor alone - no AI term
                        CHECK_THAT(sub.score, Catch::Matchers::WithinAbs(
                                                  std::clamp(100.0f - std::max(0.0f, (art.noise_floor_db + 60.0f) * 2.0f),
                                                             0.0f, 100.0f),
                                                  1e-3f));
        }
    }
}

TEST_CASE("ArtifactDetector: a steady DC offset is steady; a swinging sub-sonic mean is not (MEAS-4)", "[artifacts]")
{
    constexpr double sr = 8000.0;
    constexpr int n = 10 * 8000; // ten one-second blocks
    std::vector<float> left, right;

    // A converter-style offset: +0.005 under a 200 Hz tone, all the way through.
    generateSine(left, right, 200.0, 0.3, sr, n);
    for (int i = 0; i < n; ++i) { left[i] += 0.005f; right[i] += 0.005f; }
    ArtifactDetector steady;
    steady.reset(sr);
    const float* ch[2] = {left.data(), right.data()};
    steady.process(ch, 2, n);
    auto r = steady.getResult();
    CHECK_THAT(r.dc_offset, Catch::Matchers::WithinAbs(0.005f, 1e-4f));
    CHECK(r.dc_offset_steady);

    // Sub-sonic content swinging both ways: +0.02 for six seconds, -0.02 for
    // four. The whole-file mean (+0.004) is over the threshold, but no
    // offset holds through the file.
    generateSine(left, right, 200.0, 0.3, sr, n);
    for (int i = 0; i < n; ++i) {
        const float shift = i < 6 * 8000 ? 0.02f : -0.02f;
        left[i] += shift;
        right[i] += shift;
    }
    ArtifactDetector swinging;
    swinging.reset(sr);
    swinging.process(ch, 2, n);
    r = swinging.getResult();
    CHECK_THAT(r.dc_offset, Catch::Matchers::WithinAbs(0.004f, 1e-4f));
    CHECK_FALSE(r.dc_offset_steady);
}
