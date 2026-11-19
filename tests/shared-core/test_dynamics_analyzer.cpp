#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <khensu/analysis/DynamicsAnalyzer.h>

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

static void generateDc(std::vector<float>& left, std::vector<float>& right, float amplitude, int numSamples)
{
    left.assign(numSamples, amplitude);
    right.assign(numSamples, amplitude);
}

TEST_CASE("DynamicsAnalyzer: fresh instance with no samples returns default result", "[dynamics]")
{
    DynamicsAnalyzer da;
    da.reset(48000.0);

    auto result = da.getResult();
    CHECK(result.crest_factor_db == 0.0f);
    CHECK(result.dynamic_range_db == 0.0f);
    CHECK(result.peak_to_rms_db == 0.0f);
}

TEST_CASE("DynamicsAnalyzer: full-scale sine has crest factor near 3.01 dB", "[dynamics]")
{
    // Peak-to-RMS of a sine wave is sqrt(2) => 20*log10(sqrt(2)) ≈ 3.01 dB,
    // independent of amplitude - this is what distinguishes crest factor from
    // a simple peak or RMS reading.
    const double fs = 48000.0;
    const int dur = static_cast<int>(fs * 5.0);

    DynamicsAnalyzer da;
    da.reset(fs);

    std::vector<float> L, R;
    generateSine(L, R, 1000.0, 0.5, fs, dur);
    const float* ptrs[] = {L.data(), R.data()};
    da.process(ptrs, 2, dur);

    auto result = da.getResult();
    CHECK_THAT(result.crest_factor_db, Catch::Matchers::WithinAbs(3.01f, 0.2f));
}

TEST_CASE("DynamicsAnalyzer: constant-amplitude DC signal has near-zero crest factor and dynamic range", "[dynamics]")
{
    // A signal with no variation at all (peak == RMS, every 100ms block
    // identical) should report ~0 dB on both metrics.
    const double fs = 48000.0;
    const int dur = static_cast<int>(fs * 5.0);

    DynamicsAnalyzer da;
    da.reset(fs);

    std::vector<float> L, R;
    generateDc(L, R, 0.5f, dur);
    const float* ptrs[] = {L.data(), R.data()};
    da.process(ptrs, 2, dur);

    auto result = da.getResult();
    CHECK_THAT(result.crest_factor_db, Catch::Matchers::WithinAbs(0.0f, 0.1f));
    CHECK_THAT(result.dynamic_range_db, Catch::Matchers::WithinAbs(0.0f, 0.5f));
}

TEST_CASE("DynamicsAnalyzer: alternating loud/quiet blocks yields large dynamic range", "[dynamics]")
{
    // Alternate 5-second blocks at -6 dBFS and -32 dBFS (26 dB apart) - the
    // P95-P10 macro dynamic range should capture most of that spread, unlike
    // crest factor which only sees instantaneous peak vs. global RMS.
    const double fs = 48000.0;
    const int dur = static_cast<int>(fs * 30.0);
    const int blockSamples = static_cast<int>(fs * 5.0);

    DynamicsAnalyzer da;
    da.reset(fs);

    std::vector<float> L(dur), R(dur);
    for (int i = 0; i < dur; ++i) {
        const bool loud = (i / blockSamples) % 2 == 0;
        const double amp = loud ? 0.5 : 0.025;
        const float s = static_cast<float>(amp * std::sin(2.0 * kPi * 1000.0 * i / fs));
        L[i] = R[i] = s;
    }
    const float* ptrs[] = {L.data(), R.data()};
    da.process(ptrs, 2, dur);

    auto result = da.getResult();
    CHECK(result.dynamic_range_db > 10.0f);
}

TEST_CASE("DynamicsAnalyzer: reset() clears prior state", "[dynamics]")
{
    const double fs = 48000.0;
    const int dur = static_cast<int>(fs * 5.0);

    DynamicsAnalyzer da;
    da.reset(fs);

    std::vector<float> L, R;
    generateSine(L, R, 1000.0, 0.9, fs, dur);
    {
        const float* p[] = {L.data(), R.data()};
        da.process(p, 2, dur);
    }

    REQUIRE(da.getResult().crest_factor_db != 0.0f);

    da.reset(fs);
    auto result = da.getResult();
    CHECK(result.crest_factor_db == 0.0f);
    CHECK(result.dynamic_range_db == 0.0f);
    CHECK(result.peak_to_rms_db == 0.0f);
}

// --- AUDIT-053: crest = max per-channel peak/RMS, silent channels and LFE skipped

#include <khensu/analysis/ChannelLayout.h>

TEST_CASE("DynamicsAnalyzer: a silent channel does not inflate the crest factor", "[dynamics][audit]")
{
    // Sine on L, digital silence on R. Pooling RMS across both channels read
    // 6.02 dB (+3.01 high); loudness.crest_factor_db reads the sine's 3.01.
    const double fs = 48000.0;
    const int n = static_cast<int>(fs * 3.0);
    std::vector<float> L, R;
    generateSine(L, R, 1000.0, 0.5, fs, n);
    std::fill(R.begin(), R.end(), 0.0f);
    DynamicsAnalyzer da;
    da.reset(fs);
    const float* ptrs[] = {L.data(), R.data()};
    da.process(ptrs, 2, n);
    CHECK_THAT(da.getResult().crest_factor_db, Catch::Matchers::WithinAbs(3.0103f, 0.02f));
}

TEST_CASE("DynamicsAnalyzer: crest is the largest per-channel crest", "[dynamics][audit]")
{
    // L: steady sine (3.01 dB). R: the same sine with one 4x spike - its own
    // crest is far higher, and the max wins (as Python's per-channel max).
    const double fs = 48000.0;
    const int n = static_cast<int>(fs * 2.0);
    std::vector<float> L, R;
    generateSine(L, R, 1000.0, 0.2, fs, n);
    R[static_cast<size_t>(n / 2)] = 0.8f;
    DynamicsAnalyzer da;
    da.reset(fs);
    const float* ptrs[] = {L.data(), R.data()};
    da.process(ptrs, 2, n);
    const double rmsR = 0.2 / std::sqrt(2.0);
    CHECK_THAT(da.getResult().crest_factor_db,
               Catch::Matchers::WithinAbs(static_cast<float>(20.0 * std::log10(0.8 / rmsR)), 0.05f));
}

TEST_CASE("DynamicsAnalyzer: 5.1 LFE is excluded from the crest factor", "[dynamics][audit][layout]")
{
    const double fs = 48000.0;
    const int n = static_cast<int>(fs * 2.0);
    auto layout = ChannelLayout::fromChannelCount(6);
    REQUIRE(layout.is_ok());
    std::vector<std::vector<float>> buf(6, std::vector<float>(static_cast<size_t>(n), 0.0f));
    std::vector<float> s, unused;
    generateSine(s, unused, 1000.0, 0.3, fs, n);
    buf[0] = s;
    buf[1] = s;
    buf[3][static_cast<size_t>(n / 3)] = 0.99f; // a lone LFE spike: huge crest if counted
    buf[3][static_cast<size_t>(n / 3 + 1)] = 0.01f;
    std::vector<const float*> ptrs;
    for (auto& c : buf) ptrs.push_back(c.data());
    DynamicsAnalyzer da;
    da.reset(fs, layout.value());
    da.process(ptrs.data(), 6, n);
    CHECK_THAT(da.getResult().crest_factor_db, Catch::Matchers::WithinAbs(3.0103f, 0.02f));
}
