#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <khensu/analysis/StereoAnalyzer.h>
#include <khensu/analysis/DynamicsAnalyzer.h>
#include <khensu/analysis/ArtifactDetector.h>

#include <cmath>
#include <juce_core/juce_core.h>
#include <vector>

using namespace khensu;

static constexpr double kPi = 3.14159265358979323846;

static void makeSine(std::vector<float>& buf, double freq, double amp, double fs, int n)
{
    buf.resize(n);
    for (int i = 0; i < n; ++i)
        buf[i] = static_cast<float>(amp * std::sin(2.0 * kPi * freq * i / fs));
}

// --- StereoAnalyzer tests ---

TEST_CASE("StereoAnalyzer: identical L/R gives correlation of 1.0", "[stereo]")
{
    StereoAnalyzer sa;
    sa.reset();

    std::vector<float> L, R;
    makeSine(L, 1000.0, 0.5, 48000.0, 48000);
    R = L;
    const float* ptrs[] = {L.data(), R.data()};
    sa.process(ptrs, 2, static_cast<int>(L.size()));

    auto result = sa.getResult();
    CHECK_THAT(result.correlation, Catch::Matchers::WithinAbs(1.0f, 0.001f));
}

TEST_CASE("StereoAnalyzer: inverted R gives correlation near -1.0", "[stereo]")
{
    StereoAnalyzer sa;
    sa.reset();

    std::vector<float> L, R;
    makeSine(L, 1000.0, 0.5, 48000.0, 48000);
    R.resize(L.size());
    for (size_t i = 0; i < L.size(); ++i)
        R[i] = -L[i];

    const float* ptrs[] = {L.data(), R.data()};
    sa.process(ptrs, 2, static_cast<int>(L.size()));

    auto result = sa.getResult();
    CHECK_THAT(result.correlation, Catch::Matchers::WithinAbs(-1.0f, 0.001f));
}

TEST_CASE("StereoAnalyzer: mono input gives zero width", "[stereo]")
{
    StereoAnalyzer sa;
    sa.reset();

    std::vector<float> L;
    makeSine(L, 500.0, 0.5, 48000.0, 48000);
    std::vector<float> R = L;
    const float* ptrs[] = {L.data(), R.data()};
    sa.process(ptrs, 2, static_cast<int>(L.size()));

    auto result = sa.getResult();
    // Identical channels = zero side energy = width approaches 0
    CHECK(result.width < 0.1f);
}

TEST_CASE("StereoAnalyzer: anti-phase (fully wide) gives maximum width", "[stereo]")
{
    StereoAnalyzer sa;
    sa.reset();

    std::vector<float> L;
    makeSine(L, 500.0, 0.5, 48000.0, 48000);
    std::vector<float> R(L.size());
    for (size_t i = 0; i < L.size(); ++i)
        R[i] = -L[i];

    const float* ptrs[] = {L.data(), R.data()};
    sa.process(ptrs, 2, static_cast<int>(L.size()));

    auto result = sa.getResult();
    // L=-R means Mid=0, Side=2L → all energy in side → width approaches 1
    CHECK(result.width > 0.8f);
}

TEST_CASE("StereoAnalyzer: correlation stays between -1 and 1", "[stereo]")
{
    StereoAnalyzer sa;
    sa.reset();

    const int n = 8000;
    std::vector<float> L(n), R(n);
    for (int i = 0; i < n; ++i) {
        L[i] = static_cast<float>(0.3 * std::sin(2.0 * kPi * 800.0 * i / 48000.0));
        R[i] = static_cast<float>(0.3 * std::cos(2.0 * kPi * 800.0 * i / 48000.0));
    }
    const float* ptrs[] = {L.data(), R.data()};
    sa.process(ptrs, 2, n);

    auto result = sa.getResult();
    CHECK(result.correlation >= -1.0f);
    CHECK(result.correlation <= 1.0f);
}

// --- DynamicsAnalyzer tests ---

TEST_CASE("DynamicsAnalyzer: silence gives zero crest factor", "[dynamics]")
{
    DynamicsAnalyzer da;
    da.reset(48000.0);

    std::vector<float> L(48000, 0.0f), R(48000, 0.0f);
    const float* ptrs[] = {L.data(), R.data()};
    da.process(ptrs, 2, 48000);

    // Silence: peak and RMS both 0 → crest factor undefined, both at -90
    auto result = da.getResult();
    CHECK(result.crest_factor_db >= 0.0f); // crest factor can't be negative
}

TEST_CASE("DynamicsAnalyzer: sine has crest factor of ~3 dB", "[dynamics]")
{
    DynamicsAnalyzer da;
    da.reset(48000.0);

    std::vector<float> L, R;
    makeSine(L, 1000.0, 0.5, 48000.0, 48000 * 3);
    R = L;
    const float* ptrs[] = {L.data(), R.data()};
    da.process(ptrs, 2, static_cast<int>(L.size()));

    auto result = da.getResult();
    // Sine wave: peak = A, RMS = A/sqrt(2), crest factor = 20*log10(sqrt(2)) ≈ 3.01 dB
    CHECK_THAT(result.crest_factor_db, Catch::Matchers::WithinAbs(3.01f, 0.5f));
}

// --- ArtifactDetector tests ---

TEST_CASE("ArtifactDetector: signal at 1.0 is detected as clipping", "[artifacts]")
{
    ArtifactDetector ad;
    ad.reset(48000.0);

    std::vector<float> L(1000, 1.0f), R(1000, 1.0f);
    const float* ptrs[] = {L.data(), R.data()};
    ad.process(ptrs, 2, 1000);

    auto result = ad.getResult();
    CHECK(result.has_clipping);
    CHECK(result.clipping_frame_count > 0);
}

TEST_CASE("ArtifactDetector: signal at 0.5 is not clipping", "[artifacts]")
{
    ArtifactDetector ad;
    ad.reset(48000.0);

    std::vector<float> L(1000, 0.5f), R(1000, 0.5f);
    const float* ptrs[] = {L.data(), R.data()};
    ad.process(ptrs, 2, 1000);

    auto result = ad.getResult();
    CHECK_FALSE(result.has_clipping);
    CHECK(result.clipping_frame_count == 0);
}

TEST_CASE("ArtifactDetector: silence gives noise floor near -90 dB", "[artifacts]")
{
    ArtifactDetector ad;
    ad.reset(48000.0);

    std::vector<float> L(48000, 0.0f), R(48000, 0.0f);
    const float* ptrs[] = {L.data(), R.data()};
    ad.process(ptrs, 2, 48000);

    auto result = ad.getResult();
    CHECK(result.noise_floor_db <= -80.0f);
}

TEST_CASE("ArtifactDetector: clipping_frame_count matches number of clipped samples", "[artifacts]")
{
    ArtifactDetector ad;
    ad.reset(48000.0);

    // 100 clipping samples followed by 900 non-clipping
    std::vector<float> L(1000, 0.5f), R(1000, 0.5f);
    for (int i = 0; i < 100; ++i) {
        L[i] = 1.0f;
        R[i] = 1.0f;
    }

    const float* ptrs[] = {L.data(), R.data()};
    ad.process(ptrs, 2, 1000);

    auto result = ad.getResult();
    CHECK(result.clipping_frame_count == 100);
}

// --- AQ-01: front-pair scoping for surround layouts -----------------------

#include <khensu/analysis/ChannelLayout.h>

TEST_CASE("StereoAnalyzer: 5.1 imaging is measured from the front pair", "[stereo][layout]")
{
    // Front L/R are hard-panned opposites (correlation -1). The surround and
    // LFE channels carry unrelated content that must not influence the result:
    // these metrics are explicitly front-pair imaging, not a whole-field metric.
    const int n = 48000;
    const double fs = 48000.0;

    auto layoutR = ChannelLayout::fromChannelCount(6);
    REQUIRE(layoutR.is_ok());

    std::vector<std::vector<float>> buf(6, std::vector<float>(static_cast<size_t>(n), 0.0f));
    for (int i = 0; i < n; ++i) {
        const float s = static_cast<float>(0.5 * std::sin(2.0 * kPi * 1000.0 * i / fs));
        buf[0][static_cast<size_t>(i)] = s;    // L
        buf[1][static_cast<size_t>(i)] = -s;   // R (anti-phase with L)
        buf[3][static_cast<size_t>(i)] = 0.4f; // LFE, DC-ish
        buf[4][static_cast<size_t>(i)] = static_cast<float>(0.3 * std::sin(2.0 * kPi * 250.0 * i / fs));
    }
    std::vector<const float*> ptrs;
    for (int c = 0; c < 6; ++c) ptrs.push_back(buf[static_cast<size_t>(c)].data());

    StereoAnalyzer sa;
    sa.reset(layoutR.value());
    sa.process(ptrs.data(), 6, n);

    // Anti-phase front pair -> correlation near -1, regardless of other channels.
    CHECK(sa.getResult().correlation < -0.9f);
}

TEST_CASE("StereoAnalyzer: stereo layout matches the legacy no-arg reset", "[stereo][layout]")
{
    const int n = 48000;
    const double fs = 48000.0;
    std::vector<float> L(static_cast<size_t>(n)), R(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        L[static_cast<size_t>(i)] = static_cast<float>(0.5 * std::sin(2.0 * kPi * 440.0 * i / fs));
        R[static_cast<size_t>(i)] = static_cast<float>(0.4 * std::sin(2.0 * kPi * 440.0 * i / fs));
    }
    const float* ptrs[] = {L.data(), R.data()};

    StereoAnalyzer legacy;
    legacy.reset();
    legacy.process(ptrs, 2, n);

    auto layout = ChannelLayout::fromChannelCount(2);
    REQUIRE(layout.is_ok());
    StereoAnalyzer viaLayout;
    viaLayout.reset(layout.value());
    viaLayout.process(ptrs, 2, n);

    CHECK_THAT(viaLayout.getResult().correlation,
               Catch::Matchers::WithinAbs(legacy.getResult().correlation, 1e-6));
    CHECK_THAT(viaLayout.getResult().width,
               Catch::Matchers::WithinAbs(legacy.getResult().width, 1e-6));
}

TEST_CASE("StereoAnalyzer: width scale - uncorrelated L/R reads ~1, typical mix in genre range", "[stereo][audit]")
{
    // Independent noise in L and R (correlation ~0) is the widest a normal
    // mix gets without anti-phase: width ~1.0 (the old S/(M+S) read 0.5,
    // so genre minimums of 0.5-0.7 were unreachable).
    juce::Random rng(5);
    const int n = 96000;
    std::vector<float> L(n), R(n), C(n);
    for (int i = 0; i < n; ++i) {
        L[(size_t)i] = rng.nextFloat() - 0.5f;
        R[(size_t)i] = rng.nextFloat() - 0.5f;
        C[(size_t)i] = rng.nextFloat() - 0.5f;
    }
    StereoAnalyzer a;
    a.reset();
    const float* p1[] = {L.data(), R.data()};
    a.process(p1, 2, n);
    CHECK(std::abs(a.getResult().width - 1.0f) < 0.05f);

    // A mix with correlation ~0.6 (60% shared centre, 40% independent sides)
    // reads ~0.4-0.5 - inside common genre ranges such as 0.4-0.8.
    std::vector<float> L2(n), R2(n);
    for (int i = 0; i < n; ++i) {
        L2[(size_t)i] = 0.775f * C[(size_t)i] + 0.632f * L[(size_t)i];
        R2[(size_t)i] = 0.775f * C[(size_t)i] + 0.632f * R[(size_t)i];
    }
    StereoAnalyzer b;
    b.reset();
    const float* p2[] = {L2.data(), R2.data()};
    b.process(p2, 2, n);
    const auto res = b.getResult();
    CHECK(res.correlation > 0.5f);
    CHECK(res.correlation < 0.7f);
    CHECK(res.width > 0.35f);
    CHECK(res.width < 0.5f);
}
