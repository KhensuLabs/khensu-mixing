#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <khensu/analysis/SpectralAnalyzer.h>

#include <cmath>
#include <vector>

using namespace khensu;

static constexpr double kPi = 3.14159265358979323846;

static void generateStereoSine(std::vector<float>& L, std::vector<float>& R, double freq, double amp, double fs, int n)
{
    L.resize(n);
    R.resize(n);
    for (int i = 0; i < n; ++i) {
        const float s = static_cast<float>(amp * std::sin(2.0 * kPi * freq * i / fs));
        L[i] = R[i] = s;
    }
}

TEST_CASE("SpectralAnalyzer: silence gives all bands near -90 dB", "[spectral]")
{
    SpectralAnalyzer sa;
    sa.reset(48000.0, 2);

    std::vector<float> L(48000, 0.0f), R(48000, 0.0f);
    const float* ptrs[] = {L.data(), R.data()};
    sa.process(ptrs, 2, 48000);

    auto result = sa.getResult();
    for (float rms : result.rms_per_octave_db)
        CHECK(rms <= -60.0f);
    CHECK_THAT(result.centroid_hz, Catch::Matchers::WithinAbs(0.0f, 1.0f));
}

TEST_CASE("SpectralAnalyzer: 1kHz sine concentrates energy in 1kHz band", "[spectral]")
{
    SpectralAnalyzer sa;
    sa.reset(48000.0, 2);

    std::vector<float> L, R;
    generateStereoSine(L, R, 1000.0, 0.5, 48000.0, 48000 * 3);
    const float* ptrs[] = {L.data(), R.data()};
    sa.process(ptrs, 2, static_cast<int>(L.size()));

    auto result = sa.getResult();
    // Band 5 = 1kHz. Should be highest among all bands.
    const float band1k = result.rms_per_octave_db[5];
    for (int b = 0; b < 10; ++b) {
        if (b != 5) CHECK(band1k > result.rms_per_octave_db[b] - 3.0f); // adjacent bands may bleed a little
    }
}

TEST_CASE("SpectralAnalyzer: 100Hz sine concentrates energy in bass band", "[spectral]")
{
    SpectralAnalyzer sa;
    sa.reset(48000.0, 2);

    std::vector<float> L, R;
    generateStereoSine(L, R, 100.0, 0.5, 48000.0, 48000 * 5);
    const float* ptrs[] = {L.data(), R.data()};
    sa.process(ptrs, 2, static_cast<int>(L.size()));

    auto result = sa.getResult();
    // 100Hz sits in band 1 (62.5Hz) or band 2 (125Hz)
    // Either way, balance_low should dominate
    CHECK(result.balance_low > result.balance_mid);
    CHECK(result.balance_low > result.balance_high);
}

TEST_CASE("SpectralAnalyzer: 8kHz sine concentrates energy in high band", "[spectral]")
{
    SpectralAnalyzer sa;
    sa.reset(48000.0, 2);

    std::vector<float> L, R;
    generateStereoSine(L, R, 8000.0, 0.5, 48000.0, 48000 * 3);
    const float* ptrs[] = {L.data(), R.data()};
    sa.process(ptrs, 2, static_cast<int>(L.size()));

    auto result = sa.getResult();
    CHECK(result.balance_high > result.balance_low);
}

TEST_CASE("SpectralAnalyzer: band energies are non-negative", "[spectral]")
{
    SpectralAnalyzer sa;
    sa.reset(48000.0, 2);

    // Broadband noise (approximate via many frequencies)
    const int n = 48000 * 2;
    std::vector<float> L(n), R(n);
    for (int i = 0; i < n; ++i) {
        // Simple pseudo-noise using sum of sines at many frequencies
        float s = 0.0f;
        for (double f : {100.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0}) {
            s += static_cast<float>(0.05 * std::sin(2.0 * kPi * f * i / 48000.0));
        }
        L[i] = R[i] = s;
    }
    const float* ptrs[] = {L.data(), R.data()};
    sa.process(ptrs, 2, n);

    auto result = sa.getResult();
    CHECK(result.balance_low >= 0.0f);
    CHECK(result.balance_mid >= 0.0f);
    CHECK(result.balance_high >= 0.0f);
    CHECK(result.centroid_hz >= 0.0f);
}

TEST_CASE("SpectralAnalyzer: scalar and SIMD paths produce identical output", "[spectral]")
{
    // On x86 this compares the SSE2 fast path against the portable scalar path;
    // on arm64/other targets both selectors resolve to the scalar path, so this
    // still guards against a scalar regression. AQ-03: the arm64 slice of the
    // macOS universal build only ever runs the scalar path.
    std::vector<float> L, R;
    generateStereoSine(L, R, 1234.0, 0.4, 48000.0, 48000 * 3);
    const float* ptrs[] = {L.data(), R.data()};

    SpectralAnalyzer simd;
    simd.setUseSimd(true);
    simd.reset(48000.0, 2);
    simd.process(ptrs, 2, static_cast<int>(L.size()));
    const auto rSimd = simd.getResult();

    SpectralAnalyzer scalar;
    scalar.setUseSimd(false);
    scalar.reset(48000.0, 2);
    scalar.process(ptrs, 2, static_cast<int>(L.size()));
    const auto rScalar = scalar.getResult();

    REQUIRE(rSimd.fft_mid_magnitudes.size() == rScalar.fft_mid_magnitudes.size());
    REQUIRE(rSimd.fft_side_magnitudes.size() == rScalar.fft_side_magnitudes.size());

    // The two paths perform the same IEEE-754 single-precision multiply/add, so
    // results are bit-identical; assert with a negligible tolerance to stay
    // robust against float-contraction on non-MSVC toolchains.
    for (size_t k = 0; k < rSimd.fft_mid_magnitudes.size(); ++k) {
        CHECK_THAT(rSimd.fft_mid_magnitudes[k],
                   Catch::Matchers::WithinAbs(rScalar.fft_mid_magnitudes[k], 1e-3));
        CHECK_THAT(rSimd.fft_side_magnitudes[k],
                   Catch::Matchers::WithinAbs(rScalar.fft_side_magnitudes[k], 1e-3));
    }
    for (int b = 0; b < SpectralAnalyzer::kNumBands; ++b) {
        CHECK_THAT(rSimd.rms_per_octave_db[static_cast<size_t>(b)],
                   Catch::Matchers::WithinAbs(rScalar.rms_per_octave_db[static_cast<size_t>(b)], 1e-3));
        CHECK_THAT(rSimd.rms_mid_per_octave_db[static_cast<size_t>(b)],
                   Catch::Matchers::WithinAbs(rScalar.rms_mid_per_octave_db[static_cast<size_t>(b)], 1e-3));
        CHECK_THAT(rSimd.rms_side_per_octave_db[static_cast<size_t>(b)],
                   Catch::Matchers::WithinAbs(rScalar.rms_side_per_octave_db[static_cast<size_t>(b)], 1e-3));
    }
    CHECK_THAT(rSimd.centroid_hz, Catch::Matchers::WithinAbs(rScalar.centroid_hz, 1e-3));
}

TEST_CASE("SpectralAnalyzer: spectral centroid is above 0 for non-silence", "[spectral]")
{
    SpectralAnalyzer sa;
    sa.reset(48000.0, 2);

    std::vector<float> L, R;
    generateStereoSine(L, R, 1000.0, 0.5, 48000.0, 48000 * 2);
    const float* ptrs[] = {L.data(), R.data()};
    sa.process(ptrs, 2, static_cast<int>(L.size()));

    auto result = sa.getResult();
    CHECK(result.centroid_hz > 0.0f);
}

// --- AQ-01: LFE exclusion from the analysed mono mix ----------------------

#include <khensu/analysis/ChannelLayout.h>

TEST_CASE("SpectralAnalyzer: 5.1 LFE is excluded from the mono mix", "[spectral][layout]")
{
    // A 5.1 file whose front channels carry a bright 4 kHz tone and whose LFE
    // carries loud sub content. Excluding LFE (BS.1770 treats it as a separate
    // effects channel) must keep the centroid up where the musical content is.
    const int n = 48000 * 3;
    const double fs = 48000.0;

    auto layoutR = ChannelLayout::fromChannelCount(6);
    REQUIRE(layoutR.is_ok());
    const auto layout = layoutR.value();

    std::vector<std::vector<float>> buf(6, std::vector<float>(static_cast<size_t>(n), 0.0f));
    for (int i = 0; i < n; ++i) {
        const float hi = static_cast<float>(0.5 * std::sin(2.0 * kPi * 4000.0 * i / fs));
        const float sub = static_cast<float>(0.9 * std::sin(2.0 * kPi * 35.0 * i / fs));
        buf[0][static_cast<size_t>(i)] = hi;  // L
        buf[1][static_cast<size_t>(i)] = hi;  // R
        buf[3][static_cast<size_t>(i)] = sub; // LFE
    }
    std::vector<const float*> ptrs;
    for (int c = 0; c < 6; ++c) ptrs.push_back(buf[static_cast<size_t>(c)].data());

    SpectralAnalyzer withLayout;
    withLayout.reset(fs, layout);
    withLayout.process(ptrs.data(), 6, n);
    const auto excluded = withLayout.getResult();

    // Same audio, but treated as an unspecified 6-channel blob (LFE included).
    SpectralAnalyzer legacy;
    legacy.reset(fs, ChannelLayout::unspecified(6));
    legacy.process(ptrs.data(), 6, n);
    const auto included = legacy.getResult();

    // Including the LFE drags the centroid down; excluding it must be higher.
    CHECK(excluded.centroid_hz > included.centroid_hz);
    // And the excluded mix must not be dominated by the sub band.
    CHECK(excluded.balance_low < included.balance_low);
}

TEST_CASE("SpectralAnalyzer: stereo results are unchanged by the layout overload", "[spectral][layout]")
{
    const int n = 48000 * 2;
    std::vector<float> L, R;
    generateStereoSine(L, R, 1000.0, 0.5, 48000.0, n);
    const float* ptrs[] = {L.data(), R.data()};

    SpectralAnalyzer viaCount;
    viaCount.reset(48000.0, 2);
    viaCount.process(ptrs, 2, n);

    auto layout = ChannelLayout::fromChannelCount(2);
    REQUIRE(layout.is_ok());
    SpectralAnalyzer viaLayout;
    viaLayout.reset(48000.0, layout.value());
    viaLayout.process(ptrs, 2, n);

    CHECK_THAT(viaLayout.getResult().centroid_hz,
               Catch::Matchers::WithinAbs(viaCount.getResult().centroid_hz, 1e-3));
    CHECK_THAT(viaLayout.getResult().balance_low,
               Catch::Matchers::WithinAbs(viaCount.getResult().balance_low, 1e-6));
}

// --- AUDIT-009 / 054 / 088: one tilt + centroid definition across languages --

#include "TestNoise.h"

namespace {
AnalysisResult::SpectralData analyseMono(const std::vector<float>& x, double fs)
{
    SpectralAnalyzer sa;
    sa.reset(fs, 2);
    const float* ptrs[] = {x.data(), x.data()};
    sa.process(ptrs, 2, static_cast<int>(x.size()));
    return sa.getResult();
}
} // namespace

TEST_CASE("SpectralAnalyzer: tilt is octave-band power (pink ~0, white ~+3, brown ~-3 dB/oct)", "[spectral][audit]")
{
    // dsp_metrics.spectral_tilt_db_per_octave's definition. The old per-bin
    // density slope read pink noise at -3.05 dB/oct (3 dB/oct below Python).
    for (double fs : {44100.0, 48000.0}) {
        INFO("fs " << fs);
        const auto pink = analyseMono(test::colouredNoise(19, 1.0, 11u), fs);
        const auto white = analyseMono(test::colouredNoise(19, 0.0, 12u), fs);
        const auto brown = analyseMono(test::colouredNoise(19, 2.0, 13u), fs);
        CHECK_THAT(pink.tilt_db_per_octave, Catch::Matchers::WithinAbs(0.0f, 0.35f));
        CHECK_THAT(white.tilt_db_per_octave, Catch::Matchers::WithinAbs(3.01f, 0.35f));
        CHECK_THAT(brown.tilt_db_per_octave, Catch::Matchers::WithinAbs(-3.01f, 0.5f));
    }
}

TEST_CASE("SpectralAnalyzer: centroid is librosa's magnitude-weighted per-frame mean", "[spectral][audit]")
{
    // Magnitude-weighted over linear bins: white noise sits at ~fs/4, pink
    // (|X| ~ f^-1/2) at ~fs/6. The old power-weighted mean of 10 octave-band
    // centres read pink at ~3 kHz.
    const double fs = 48000.0;
    const auto white = analyseMono(test::colouredNoise(19, 0.0, 21u), fs);
    const auto pink = analyseMono(test::colouredNoise(19, 1.0, 22u), fs);
    CHECK_THAT(white.centroid_hz, Catch::Matchers::WithinAbs(12000.0f, 250.0f));
    CHECK_THAT(pink.centroid_hz, Catch::Matchers::WithinAbs(8000.0f, 400.0f));

    // A pure tone's centroid is (near) the tone itself.
    std::vector<float> L, R;
    generateStereoSine(L, R, 1000.0, 0.5, fs, 48000 * 2);
    const auto tone = analyseMono(L, fs);
    CHECK_THAT(tone.centroid_hz, Catch::Matchers::WithinAbs(1000.0f, 60.0f));
}

TEST_CASE("SpectralAnalyzer: a file shorter than one FFT frame keeps the same units", "[spectral][audit]")
{
    // AUDIT-088: under 4096 samples there is no full FFT frame, and the
    // fallback used to report biquad band POWER instead of the FFT's mean
    // per-bin power - a different scale for the same tone.
    const double fs = 48000.0;
    std::vector<float> longL, longR, shortL, shortR;
    generateStereoSine(longL, longR, 1000.0, 0.5, fs, 48000 * 2);
    generateStereoSine(shortL, shortR, 1000.0, 0.5, fs, 3000);
    const auto longR1 = analyseMono(longL, fs);
    const auto shortR1 = analyseMono(shortL, fs);

    REQUIRE(shortR1.rms_per_octave_db.size() == 10);
    CHECK_THAT(shortR1.rms_per_octave_db[5], Catch::Matchers::WithinAbs(longR1.rms_per_octave_db[5], 1.5f));
    CHECK_THAT(shortR1.centroid_hz, Catch::Matchers::WithinAbs(1000.0f, 120.0f));
    CHECK(shortR1.fft_mid_magnitudes.size() == 2048);
    CHECK(shortR1.fft_mid_magnitudes[static_cast<size_t>(1000.0 / (fs / 4096.0))] > -40.0f);
}
