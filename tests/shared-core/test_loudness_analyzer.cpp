#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <khensu/analysis/LoudnessAnalyzer.h>
#include <khensu/analysis/NativeMeters.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

using namespace khensu;

static constexpr double kPi = 3.14159265358979323846;

// Generate a stereo sine wave at given frequency and amplitude
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

static void generateSilence(std::vector<float>& left, std::vector<float>& right, int numSamples)
{
    left.assign(numSamples, 0.0f);
    right.assign(numSamples, 0.0f);
}

TEST_CASE("LoudnessAnalyzer: silence returns gated-out LUFS (-70)", "[loudness]")
{
    LoudnessAnalyzer la;
    la.reset(48000.0, 2);

    std::vector<float> L, R;
    generateSilence(L, R, 48000 * 5); // 5 seconds of silence
    const float* ptrs[] = {L.data(), R.data()};
    la.process(ptrs, 2, static_cast<int>(L.size()));

    auto result = la.getResult();
    // All blocks below absolute gate: integrated = -70 (our sentinel)
    CHECK(result.lufs_integrated <= -60.0f);
}

TEST_CASE("LoudnessAnalyzer: 1kHz sine at -20dBFS reads near expected LUFS", "[loudness]")
{
    // At 1kHz, K-weighting is approximately flat (0 dB effect).
    // Stereo 1kHz sine at amplitude A: LUFS ≈ -0.691 + 10*log10(2*(A/sqrt(2))^2) = -0.691 + 20*log10(A)
    // For A = 0.1 (-20 dBFS peak): expected LUFS ≈ -0.691 + 20*log10(0.1) = -0.691 - 20 = -20.691
    // Tolerance: +/-2 LU (allows for K-weighting gain at 1kHz ~0dB + gating effects on short signal)
    const double fs = 48000.0;
    const double amp = 0.1;                      // -20 dBFS peak
    const int dur = static_cast<int>(fs * 10.0); // 10 seconds ensures enough gated windows

    LoudnessAnalyzer la;
    la.reset(fs, 2);

    std::vector<float> L, R;
    generateSine(L, R, 1000.0, amp, fs, dur);
    const float* ptrs[] = {L.data(), R.data()};
    la.process(ptrs, 2, dur);

    auto result = la.getResult();
    CHECK_THAT(result.lufs_integrated, Catch::Matchers::WithinAbs(-20.691f, 2.0f));
}

TEST_CASE("LoudnessAnalyzer: full-scale sine has positive PLR (peak > loudness)", "[loudness]")
{
    const double fs = 48000.0;
    const int dur = static_cast<int>(fs * 5.0);

    LoudnessAnalyzer la;
    la.reset(fs, 2);

    std::vector<float> L, R;
    generateSine(L, R, 1000.0, 1.0, fs, dur);
    const float* ptrs[] = {L.data(), R.data()};
    la.process(ptrs, 2, dur);

    auto result = la.getResult();
    // PLR = true peak - integrated. A full-scale STEREO 1 kHz sine reads
    // ~0.0 LUFS (BS.1770: 0 dBFS in one channel = -3.01 LUFS) with a ~0 dBTP
    // true peak, so PLR ~ 0. (The old "> 0" check only held because the RLB
    // filter read 0.04 LU low.)
    CHECK(std::abs(result.plr) < 0.1f);
}

TEST_CASE("LoudnessAnalyzer: true peak near 0 dBTP for full-scale sine", "[loudness]")
{
    const double fs = 48000.0;
    const int dur = static_cast<int>(fs * 3.0);

    LoudnessAnalyzer la;
    la.reset(fs, 2);

    std::vector<float> L, R;
    generateSine(L, R, 1000.0, 1.0, fs, dur);
    const float* ptrs[] = {L.data(), R.data()};
    la.process(ptrs, 2, dur);

    auto result = la.getResult();
    CHECK(result.true_peak_dbtp > -1.0f); // near 0 dBTP
    CHECK(result.true_peak_dbtp <= 0.1f); // not exceeding 0 dBFS (sample-based)
}

TEST_CASE("LoudnessAnalyzer: louder signal has higher LUFS", "[loudness]")
{
    const double fs = 48000.0;
    const int dur = static_cast<int>(fs * 10.0);

    LoudnessAnalyzer la1, la2;
    la1.reset(fs, 2);
    la2.reset(fs, 2);

    std::vector<float> L1, R1, L2, R2;
    generateSine(L1, R1, 1000.0, 0.1, fs, dur);  // -20 dBFS
    generateSine(L2, R2, 1000.0, 0.01, fs, dur); // -40 dBFS

    {
        const float* p[] = {L1.data(), R1.data()};
        la1.process(p, 2, dur);
    }
    {
        const float* p[] = {L2.data(), R2.data()};
        la2.process(p, 2, dur);
    }

    CHECK(la1.getResult().lufs_integrated > la2.getResult().lufs_integrated);
}

TEST_CASE("LoudnessAnalyzer: LRA is non-negative", "[loudness]")
{
    const double fs = 48000.0;
    const int dur = static_cast<int>(fs * 60.0); // 1 minute for stable LRA

    LoudnessAnalyzer la;
    la.reset(fs, 2);

    // Alternating quiet and loud sections to create LRA
    std::vector<float> L(dur), R(dur);
    for (int i = 0; i < dur; ++i) {
        const double amp = (i / static_cast<int>(fs * 5)) % 2 == 0 ? 0.5 : 0.05;
        const float s = static_cast<float>(amp * std::sin(2.0 * kPi * 1000.0 * i / fs));
        L[i] = R[i] = s;
    }

    const float* ptrs[] = {L.data(), R.data()};
    la.process(ptrs, 2, dur);

    auto result = la.getResult();
    CHECK(result.lra >= 0.0f);
}

TEST_CASE("LoudnessAnalyzer: short-term max >= integrated for constant signal", "[loudness]")
{
    const double fs = 48000.0;
    const int dur = static_cast<int>(fs * 10.0);

    LoudnessAnalyzer la;
    la.reset(fs, 2);

    std::vector<float> L, R;
    generateSine(L, R, 1000.0, 0.1, fs, dur);
    const float* ptrs[] = {L.data(), R.data()};
    la.process(ptrs, 2, dur);

    auto result = la.getResult();
    // For a constant-level signal, short-term max should be close to integrated
    CHECK(result.lufs_short_term_max >= result.lufs_integrated - 2.0f);
}

// --- AQ-01: BS.1770 multichannel weighting -------------------------------

#include <khensu/analysis/ChannelLayout.h>

namespace {

/// Planar multichannel buffers: a sine in exactly `activeChannels`, silence
/// elsewhere. `ptrs` points into `storage`, so keep the struct alive while used.
struct MultiChannelSignal {
    std::vector<std::vector<float>> storage;
    std::vector<const float*> ptrs;
};

MultiChannelSignal makeSignal(int numChannels, int n, double fs, double freq, double amp,
                              const std::vector<int>& activeChannels)
{
    MultiChannelSignal s;
    s.storage.assign(static_cast<size_t>(numChannels), std::vector<float>(static_cast<size_t>(n), 0.0f));
    for (int c : activeChannels)
        for (int i = 0; i < n; ++i)
            s.storage[static_cast<size_t>(c)][static_cast<size_t>(i)] =
                static_cast<float>(amp * std::sin(2.0 * kPi * freq * i / fs));
    for (int c = 0; c < numChannels; ++c)
        s.ptrs.push_back(s.storage[static_cast<size_t>(c)].data());
    return s;
}

// Takes a non-const reference: process() wants `const float**`, and .data() on
// a const vector<const float*> yields `const float* const*`.
float measureLufs(const ChannelLayout& layout, MultiChannelSignal& sig, int n, double fs)
{
    LoudnessAnalyzer la;
    la.reset(fs, layout);
    la.process(sig.ptrs.data(), layout.numChannels, n);
    return la.getResult().lufs_integrated;
}

} // namespace

TEST_CASE("LoudnessAnalyzer: mono and stereo are unchanged by the layout overload", "[loudness][layout]")
{
    // The regression guard for the whole change: every weight in mono/stereo is
    // 1.0 and neither has an LFE channel, so the layout overload must produce
    // exactly what the channel-count overload always produced.
    const int n = 48000 * 4;
    const double fs = 48000.0;

    for (int ch : {1, 2}) {
        auto layout = ChannelLayout::fromChannelCount(ch);
        REQUIRE(layout.is_ok());
        std::vector<int> active;
        for (int c = 0; c < ch; ++c) active.push_back(c);
        auto sig = makeSignal(ch, n, fs, 1000.0, 0.5, active);

        LoudnessAnalyzer legacy;
        legacy.reset(fs, ch); // old API
        legacy.process(sig.ptrs.data(), ch, n);

        INFO("channel count " << ch);
        CHECK_THAT(measureLufs(layout.value(), sig, n, fs),
                   Catch::Matchers::WithinAbs(legacy.getResult().lufs_integrated, 1e-6));
    }
}

TEST_CASE("LoudnessAnalyzer: 5.1 LFE is excluded from integrated loudness", "[loudness][layout]")
{
    const int n = 48000 * 4;
    const double fs = 48000.0;
    auto layout = ChannelLayout::fromChannelCount(6);
    REQUIRE(layout.is_ok());

    auto lr = makeSignal(6, n, fs, 1000.0, 0.5, {0, 1});
    const float lufsLR = measureLufs(layout.value(), lr, n, fs);

    // Same L+R, plus loud content in the LFE channel (index 3).
    auto lrPlusLfe = makeSignal(6, n, fs, 1000.0, 0.5, {0, 1, 3});
    const float lufsWithLfe = measureLufs(layout.value(), lrPlusLfe, n, fs);

    // BS.1770 excludes LFE: adding it must not move integrated loudness.
    CHECK_THAT(lufsWithLfe, Catch::Matchers::WithinAbs(lufsLR, 1e-4));
}

TEST_CASE("LoudnessAnalyzer: 5.1 LFE-only content reads as silence", "[loudness][layout]")
{
    const int n = 48000 * 4;
    const double fs = 48000.0;
    auto layout = ChannelLayout::fromChannelCount(6);
    REQUIRE(layout.is_ok());

    auto lfeOnly = makeSignal(6, n, fs, 60.0, 0.7, {3});
    // Every contributing weight is zero, so the gate floors the result.
    CHECK(measureLufs(layout.value(), lfeOnly, n, fs) <= -70.0f);
}

TEST_CASE("LoudnessAnalyzer: surround channels carry the +1.5 dB BS.1770 weight", "[loudness][layout]")
{
    const int n = 48000 * 4;
    const double fs = 48000.0;
    auto layout = ChannelLayout::fromChannelCount(6);
    REQUIRE(layout.is_ok());

    // Identical signal in front-left (weight 1.0) vs left-surround (weight 1.41).
    auto front = makeSignal(6, n, fs, 1000.0, 0.5, {0});
    auto surround = makeSignal(6, n, fs, 1000.0, 0.5, {4});

    const float lufsFront = measureLufs(layout.value(), front, n, fs);
    const float lufsSurround = measureLufs(layout.value(), surround, n, fs);

    CHECK_THAT(lufsSurround - lufsFront, Catch::Matchers::WithinAbs(1.5f, 0.02f));
}

TEST_CASE("LoudnessAnalyzer: 7.1 back channels are weighted like surrounds", "[loudness][layout]")
{
    const int n = 48000 * 4;
    const double fs = 48000.0;
    auto layout = ChannelLayout::fromChannelCount(8);
    REQUIRE(layout.is_ok());

    auto front = makeSignal(8, n, fs, 1000.0, 0.5, {0});
    auto back = makeSignal(8, n, fs, 1000.0, 0.5, {6}); // LeftBack
    CHECK_THAT(measureLufs(layout.value(), back, n, fs) - measureLufs(layout.value(), front, n, fs),
               Catch::Matchers::WithinAbs(1.5f, 0.02f));
}

// ===========================================================================
// AQ-07: Compliance and standard-vector validation suite
// ===========================================================================
// Standards Implemented:
// - Loudness & True Peak: ITU-R BS.1770-4 / ITU-R BS.1770-5 (Annex 1 & 2)
// - Loudness Range (LRA): EBU Tech 3342 v2.0
//
// Tolerance Rationale:
// - Loudness Measurements: ITU-R BS.2217 compliance calls for a tolerance of
//   ±0.1 LU/LKFS for integrated loudness on synthetic reference signals.
// - Loudness Range (LRA): EBU Tech 3342 defines a tolerance of ±1.0 LU, but
//   for our deterministic synthetic signals we target a tight ±0.5 LU.
// - True Peak (dBTP): ITU-R BS.1770 Annex 2 shows that a 4x oversampler has a
//   maximum theoretical under-read of 0.68 dB. A windowed-sinc interpolator
//   exceeds this minimum, so we assert within -0.1 dB to +0.05 dB of 0.0 dBTP.
// ===========================================================================

TEST_CASE("LoudnessAnalyzer: EBU Tech 3341 cases 1-2 - level linearity at -23 and -33 dBFS", "[loudness][compliance]")
{
    // EBU Tech 3341 test 1/2: a stereo 1 kHz sine at -23 / -33 dBFS (peak,
    // each channel) reads -23.0 / -33.0 LUFS +/-0.1, and a 10 dB level change
    // moves integrated loudness by exactly 10 LU. (Replaces an assertion-free
    // SUCCEED() that "reported" compliance without measuring anything.)
    const double fs = 48000.0;
    const int dur = static_cast<int>(fs * 20.0);
    auto measure = [&](double dbfs) {
        LoudnessAnalyzer la;
        la.reset(fs, 2);
        std::vector<float> L, R;
        generateSine(L, R, 1000.0, std::pow(10.0, dbfs / 20.0), fs, dur);
        const float* ptrs[] = {L.data(), R.data()};
        la.process(ptrs, 2, dur);
        return la.getResult();
    };
    const auto a = measure(-23.0);
    const auto b = measure(-33.0);
    CHECK_THAT(a.lufs_integrated, Catch::Matchers::WithinAbs(-23.0f, 0.1f));
    CHECK_THAT(b.lufs_integrated, Catch::Matchers::WithinAbs(-33.0f, 0.1f));
    CHECK_THAT(a.lufs_integrated - b.lufs_integrated, Catch::Matchers::WithinAbs(10.0f, 0.01f));
    // A steady tone has no loudness range and its short-term max equals
    // the integrated level.
    CHECK(a.lra_measured);
    CHECK_THAT(a.lra, Catch::Matchers::WithinAbs(0.0f, 0.1f));
    CHECK_THAT(a.lufs_short_term_max, Catch::Matchers::WithinAbs(a.lufs_integrated, 0.1f));
}

TEST_CASE("LoudnessAnalyzer: ITU-R BS.1770-5 / BS.2217-2 Reference Vector levels", "[loudness][compliance]")
{
    const double fs = 48000.0;
    const int dur10s = static_cast<int>(fs * 10.0);

    SECTION("Mono 1kHz sine wave at -20 dBFS peak (Center channel)")
    {
        LoudnessAnalyzer la;
        la.reset(fs, 1);
        std::vector<float> L, R;
        generateSine(L, R, 1000.0, 0.1, fs, dur10s);
        const float* ptrs[] = {L.data()};
        la.process(ptrs, 1, dur10s);

        auto result = la.getResult();
        // Mono formula: L = -0.691 + 10 * log10(1.0 * RMS^2) = -20 - 3.01 = -23.01 LUFS
        CHECK_THAT(result.lufs_integrated, Catch::Matchers::WithinAbs(-23.01f, 0.1f));
    }

    SECTION("Stereo 1kHz sine wave at -20 dBFS peak per channel (in-phase)")
    {
        LoudnessAnalyzer la;
        la.reset(fs, 2);
        std::vector<float> L, R;
        generateSine(L, R, 1000.0, 0.1, fs, dur10s);
        const float* ptrs[] = {L.data(), R.data()};
        la.process(ptrs, 2, dur10s);

        auto result = la.getResult();
        // Stereo formula: L = -0.691 + 10 * log10(1.0 * RMS_L^2 + 1.0 * RMS_R^2) = -20 LUFS
        CHECK_THAT(result.lufs_integrated, Catch::Matchers::WithinAbs(-20.0f, 0.1f));
    }

    SECTION("Stereo 1kHz sine wave at -23 dBFS peak per channel (in-phase)")
    {
        LoudnessAnalyzer la;
        la.reset(fs, 2);
        std::vector<float> L, R;
        const double amp = std::pow(10.0, -23.0 / 20.0);
        generateSine(L, R, 1000.0, amp, fs, dur10s);
        const float* ptrs[] = {L.data(), R.data()};
        la.process(ptrs, 2, dur10s);

        auto result = la.getResult();
        CHECK_THAT(result.lufs_integrated, Catch::Matchers::WithinAbs(-23.0f, 0.1f));
    }
}

TEST_CASE("LoudnessAnalyzer: ITU-R BS.1770-5 / BS.2217-2 Gating compliance", "[loudness][compliance]")
{
    const double fs = 48000.0;
    const int dur5s = static_cast<int>(fs * 5.0);

    SECTION("Absolute Gating (ignores segments below -70 LUFS)")
    {
        LoudnessAnalyzer la;
        la.reset(fs, 2);

        // 5 seconds of active signal at -20 LUFS
        std::vector<float> L1, R1;
        generateSine(L1, R1, 1000.0, 0.1, fs, dur5s);
        const float* ptrs1[] = {L1.data(), R1.data()};
        la.process(ptrs1, 2, dur5s);

        // 5 seconds of silent/low signal at -80 LUFS
        std::vector<float> L2, R2;
        generateSine(L2, R2, 1000.0, 0.0001, fs, dur5s);
        const float* ptrs2[] = {L2.data(), R2.data()};
        la.process(ptrs2, 2, dur5s);

        auto result = la.getResult();
        // Since -80 LUFS is gated out, integrated loudness is determined only by the active section
        // and the transition windows (which pull it slightly under -20.0).
        CHECK_THAT(result.lufs_integrated, Catch::Matchers::WithinAbs(-20.12f, 0.02f));
    }

    SECTION("Relative Gating (ignores segments 10 LU below absolute-gated average)")
    {
        LoudnessAnalyzer la;
        la.reset(fs, 2);

        // 5 seconds of active signal at -10 LUFS
        std::vector<float> L1, R1;
        generateSine(L1, R1, 1000.0, 0.316227766, fs, dur5s);
        const float* ptrs1[] = {L1.data(), R1.data()};
        la.process(ptrs1, 2, dur5s);

        // 5 seconds of signal at -25 LUFS (15 LU below, which falls under relative gate)
        std::vector<float> L2, R2;
        generateSine(L2, R2, 1000.0, 0.0562341325, fs, dur5s);
        const float* ptrs2[] = {L2.data(), R2.data()};
        la.process(ptrs2, 2, dur5s);

        auto result = la.getResult();
        // Since -25 LUFS is gated out by relative gate, integrated loudness remains around -10.16 LUFS
        // due to transition windows.
        CHECK_THAT(result.lufs_integrated, Catch::Matchers::WithinAbs(-10.12f, 0.02f));
    }
}

TEST_CASE("LoudnessAnalyzer: EBU Tech 3341 reference level is exact", "[loudness][compliance][audit]")
{
    // 997 Hz stereo sine at -20 dBFS peak per channel must read -20.00 LUFS
    // (K-weighting is ~0 dB at 997 Hz; two channels at -23.01 each sum to -20).
    // Exercises the RLB numerator: the old normalised [1+cos]/2 form read
    // 0.04 LU low at every sample rate.
    for (double fs : {44100.0, 48000.0, 96000.0}) {
        LoudnessAnalyzer la;
        la.reset(fs, 2);
        const int dur = static_cast<int>(fs * 10.0);
        std::vector<float> L, R;
        generateSine(L, R, 997.0, 0.1, fs, dur);
        const float* ptrs[] = {L.data(), R.data()};
        la.process(ptrs, 2, dur);
        INFO("fs " << fs);
        CHECK_THAT(la.getResult().lufs_integrated, Catch::Matchers::WithinAbs(-20.0f, 0.02f));
    }
}

TEST_CASE("LoudnessAnalyzer: material too short for LRA is flagged, not 0 LU", "[loudness][audit]")
{
    const double fs = 48000.0;
    const int dur = static_cast<int>(fs * 2.0); // under one 3 s short-term window
    LoudnessAnalyzer la;
    la.reset(fs, 2);
    std::vector<float> L, R;
    generateSine(L, R, 1000.0, 0.3, fs, dur);
    const float* ptrs[] = {L.data(), R.data()};
    la.process(ptrs, 2, dur);
    CHECK_FALSE(la.getResult().lra_measured);

    LoudnessAnalyzer notReset; // process() before reset() must not hang
    notReset.process(ptrs, 2, dur);
    SUCCEED();
}

TEST_CASE("LoudnessAnalyzer: EBU Tech 3342 v2.0 Loudness Range (LRA) compliance", "[loudness][compliance][lra]")
{
    const double fs = 48000.0;
    const int dur20s = static_cast<int>(fs * 20.0);

    SECTION("EBU Tech 3342 Case 10 (Expected LRA = 10 LU)")
    {
        LoudnessAnalyzer la;
        la.reset(fs, 2);

        std::vector<float> L1, R1;
        generateSine(L1, R1, 1000.0, 0.1, fs, dur20s); // -20 dBFS peak per channel
        const float* ptrs1[] = {L1.data(), R1.data()};
        la.process(ptrs1, 2, dur20s);

        std::vector<float> L2, R2;
        generateSine(L2, R2, 1000.0, 0.0316227766, fs, dur20s); // -30 dBFS peak per channel
        const float* ptrs2[] = {L2.data(), R2.data()};
        la.process(ptrs2, 2, dur20s);

        auto result = la.getResult();
        CHECK_THAT(result.lra, Catch::Matchers::WithinAbs(10.0f, 0.5f));
    }

    SECTION("EBU Tech 3342 Case 11 (Expected LRA = 5 LU)")
    {
        LoudnessAnalyzer la;
        la.reset(fs, 2);

        std::vector<float> L1, R1;
        generateSine(L1, R1, 1000.0, 0.1, fs, dur20s); // -20 dBFS peak per channel
        const float* ptrs1[] = {L1.data(), R1.data()};
        la.process(ptrs1, 2, dur20s);

        std::vector<float> L2, R2;
        generateSine(L2, R2, 1000.0, 0.177827941, fs, dur20s); // -15 dBFS peak per channel
        const float* ptrs2[] = {L2.data(), R2.data()};
        la.process(ptrs2, 2, dur20s);

        auto result = la.getResult();
        CHECK_THAT(result.lra, Catch::Matchers::WithinAbs(5.0f, 0.5f));
    }

    SECTION("EBU Tech 3342 LRA Gating Test (Expected LRA = 15 LU)")
    {
        LoudnessAnalyzer la;
        la.reset(fs, 2);

        // 20s at -50 LUFS (gated out by relative threshold of -26 - 20 = -46 LUFS)
        std::vector<float> L1, R1;
        generateSine(L1, R1, 1000.0, 0.00316227766, fs, dur20s);
        const float* ptrs1[] = {L1.data(), R1.data()};
        la.process(ptrs1, 2, dur20s);

        // 20s at -35 LUFS (included, represents lower percentile)
        std::vector<float> L2, R2;
        generateSine(L2, R2, 1000.0, 0.0177827941, fs, dur20s);
        const float* ptrs2[] = {L2.data(), R2.data()};
        la.process(ptrs2, 2, dur20s);

        // 20s at -20 LUFS (included, represents upper percentile)
        std::vector<float> L3, R3;
        generateSine(L3, R3, 1000.0, 0.1, fs, dur20s);
        const float* ptrs3[] = {L3.data(), R3.data()};
        la.process(ptrs3, 2, dur20s);

        // Repeat -35 and -50 to complete 100s symmetric signal
        la.process(ptrs2, 2, dur20s);
        la.process(ptrs1, 2, dur20s);

        auto result = la.getResult();
        CHECK_THAT(result.lra, Catch::Matchers::WithinAbs(15.0f, 0.5f));
    }
}

TEST_CASE("LoudnessAnalyzer: ITU-R BS.1770-5 Annex 2 True Peak conformance", "[loudness][compliance][truepeak]")
{
    const double fs = 48000.0;
    const int dur = 48000; // 1 second

    SECTION("True Peak matches sample peak when sample aligns with wave peak (0 phase)")
    {
        LoudnessAnalyzer la;
        la.reset(fs, 2);

        std::vector<float> L(dur), R(dur);
        for (int i = 0; i < dur; ++i) {
            float val = 0.0f;
            if (i % 4 == 1) val = 1.0f;
            else if (i % 4 == 3) val = -1.0f;
            L[i] = R[i] = val;
        }

        const float* ptrs[] = {L.data(), R.data()};
        la.process(ptrs, 2, dur);
        auto result = la.getResult();

        // Sample peak is 1.0 (0.0 dBFS), True Peak should be 0.0 dBTP.
        CHECK_THAT(result.true_peak_dbtp, Catch::Matchers::WithinAbs(0.0f, 0.05f));
    }

    SECTION("True Peak detects inter-sample peak when sample falls between wave peaks (45 deg phase)")
    {
        LoudnessAnalyzer la;
        la.reset(fs, 2);

        std::vector<float> L(dur), R(dur);
        const float val45 = 0.707106781f;
        for (int i = 0; i < dur; ++i) {
            float val = 0.0f;
            if (i % 4 == 0 || i % 4 == 1) val = val45;
            else val = -val45;
            L[i] = R[i] = val;
        }

        const float* ptrs[] = {L.data(), R.data()};
        la.process(ptrs, 2, dur);
        auto result = la.getResult();

        // Sample peak is 0.70710678 (-3.01 dBFS), but continuous wave peak is 1.0 (0.0 dBTP).
        // 4x oversampler will capture this inter-sample peak.
        CHECK(result.true_peak_dbtp >= -0.1f);
        CHECK(result.true_peak_dbtp <= 0.2f);
    }
}


// MEAS-5: one loudness / true-peak kernel. The desktop analyzer, the native
// meters (pinned to the Python service by test_native_meters_parity.cpp) and
// the plugin share one K-weighting and one true-peak filter, so the fix the app
// aims at -1 dBTP reads the same when the service checks the render.
TEST_CASE("MEAS-5: K-weighting has one definition, the ITU table at 48 kHz", "[loudness][meas5]")
{
    const auto k48 = analysis::kWeightingCoefficients(48000.0);
    const std::array<double, 5> ituShelf{1.53512485958697, -2.69169618940638, 1.19839281085285, -1.69065929318241,
                                         0.73248077421585};
    const std::array<double, 5> ituRlb{1.0, -2.0, 1.0, -1.99004745483398, 0.99007225036621};
    CHECK(k48.preFilter == ituShelf);
    CHECK(k48.rlbFilter == ituRlb);
    for (double sr : {44100.0, 48000.0, 96000.0}) {
        const auto shared = analysis::kWeightingCoefficients(sr);
        const auto native = native::kWeighting(sr);
        for (size_t i = 0; i < 5; ++i) {
            CHECK(native.shelf[i] == shared.preFilter[i]);
            CHECK(native.rlb[i] == shared.rlbFilter[i]);
        }
    }
}

namespace {
float analyzerTruePeak(const std::vector<float>& x, double sr)
{
    LoudnessAnalyzer la;
    la.reset(sr, 1);
    const float* ptrs[] = {x.data()};
    // Odd block sizes so the streaming history wraps mid-block.
    int off = 0;
    for (int block : {1, 37, 511, 4096}) {
        if (off >= static_cast<int>(x.size())) break;
        const int n = std::min(block, static_cast<int>(x.size()) - off);
        const float* p[] = {ptrs[0] + off};
        la.process(p, 1, n);
        off += n;
    }
    while (off < static_cast<int>(x.size())) {
        const int n = std::min(4096, static_cast<int>(x.size()) - off);
        const float* p[] = {ptrs[0] + off};
        la.process(p, 1, n);
        off += n;
    }
    return la.getResult().true_peak_dbtp;
}

double nativeTruePeakDb(const std::vector<float>& x)
{
    return 20.0 * std::log10(native::truePeakLinear(x.data(), static_cast<std::int64_t>(x.size())) + 1e-12);
}
} // namespace

TEST_CASE("MEAS-5: the analyzer's true peak equals the native meter's", "[loudness][meas5][truepeak]")
{
    const double sr = 48000.0;
    std::vector<std::vector<float>> signals;
    {
        std::vector<float> tone(48000); // 0.45 fs, the case CORE-13 was about
        for (size_t i = 0; i < tone.size(); ++i)
            tone[i] = static_cast<float>(0.5 * std::sin(2.0 * kPi * 0.45 * static_cast<double>(i) + 0.3));
        signals.push_back(std::move(tone));
    }
    {
        std::vector<float> isp(48000); // fs/4 at 45 degrees: inter-sample peak 1.0
        for (size_t i = 0; i < isp.size(); ++i)
            isp[i] = (i % 4 == 0 || i % 4 == 1) ? 0.707106781f : -0.707106781f;
        signals.push_back(std::move(isp));
    }
    {
        std::mt19937 rng(1234);
        std::normal_distribution<float> nd(0.0f, 0.2f);
        std::vector<float> noise(30000);
        for (auto& v : noise) v = nd(rng);
        signals.push_back(std::move(noise));
    }
    {
        std::vector<float> lastEdge(5000, 0.0f); // the peak sits in the last samples
        lastEdge[4998] = 0.9f;
        lastEdge[4999] = -0.9f;
        signals.push_back(std::move(lastEdge));
    }
    {
        std::vector<float> firstEdge(5000, 0.0f);
        firstEdge[0] = -0.8f;
        firstEdge[1] = 0.8f;
        signals.push_back(std::move(firstEdge));
    }
    for (size_t s = 0; s < signals.size(); ++s) {
        INFO("signal " << s);
        CHECK_THAT(analyzerTruePeak(signals[s], sr), Catch::Matchers::WithinAbs(nativeTruePeakDb(signals[s]), 0.002));
    }
}

TEST_CASE("MEAS-5: silence reads the same true-peak floor as the service", "[loudness][meas5][truepeak]")
{
    std::vector<float> silence(48000, 0.0f);
    CHECK_THAT(analyzerTruePeak(silence, 48000.0), Catch::Matchers::WithinAbs(-240.0, 0.01));
    CHECK_THAT(nativeTruePeakDb(silence), Catch::Matchers::WithinAbs(-240.0, 0.01));
}
