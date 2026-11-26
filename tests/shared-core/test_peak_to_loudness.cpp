// tests/shared-core/test_peak_to_loudness.cpp
// STU-3: PSR over time - the highest sample peak in the 3 s window minus the
// short-term loudness, and the passages under 6 dB read as squashed.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <khensu/analysis/PeakToLoudness.h>
#include <khensu/analysis/TrackTimeline.h>
#include <cmath>
#include <vector>

using namespace khensu;
using Catch::Approx;

TEST_CASE("STU-3: PSR is the window's highest peak minus the short-term loudness", "[psr]")
{
    const double hop = 0.1;
    std::vector<float> peak(100, -6.0f), st(100, -14.0f);
    peak[40] = -1.0f; // one loud hit at 4.0 s
    const auto psr = psr::series(peak, st, hop);
    REQUIRE(psr.size() == 100);
    CHECK(psr[10] == Approx(8.0f));
    // The hit counts for the 3 s window ending at each later hop (30 hops).
    CHECK(psr[40] == Approx(13.0f));
    CHECK(psr[69] == Approx(13.0f));
    CHECK(psr[70] == Approx(8.0f));
    CHECK(psr[39] == Approx(8.0f));
    CHECK(psr::at(psr, hop, 4.02) == Approx(13.0f));
    CHECK_FALSE(psr::measured(psr::at(psr, hop, 50.0)));
}

TEST_CASE("STU-3: silence has no PSR and the series follow the shorter input", "[psr]")
{
    std::vector<float> peak(20, -60.0f), st(10, -70.0f);
    st[5] = -20.0f;
    const auto psr = psr::series(peak, st, 0.1);
    REQUIRE(psr.size() == 10);
    CHECK_FALSE(psr::measured(psr[0]));
    CHECK(psr::measured(psr[5]));
    CHECK(psr::series({}, {}, 0.1).empty());
    CHECK(psr::squashedPassages(psr::series(peak, st, 0.0), 0.0).empty());
}

TEST_CASE("STU-3: squashed passages are runs under 6 dB that last", "[psr]")
{
    const double hop = 0.1;
    std::vector<float> psr(200, 9.0f);
    for (size_t i = 50; i < 100; ++i)
        psr[i] = 4.5f; // 5.0 - 10.0 s squashed
    psr[70] = 3.2f;
    for (size_t i = 150; i < 160; ++i)
        psr[i] = 5.0f; // one second: too short to call
    psr[199] = psr::kUnmeasured;
    const auto runs = psr::squashedPassages(psr, hop);
    REQUIRE(runs.size() == 1);
    CHECK(runs[0].startSec == Approx(5.0));
    CHECK(runs[0].endSec == Approx(10.0));
    CHECK(runs[0].minPsrDb == Approx(3.2f));
    CHECK(psr::squashedPassages(psr, hop, 6.0f, 0.5).size() == 2);
}

TEST_CASE("STU-3: PSR from a measured timeline separates a limited signal from a dynamic one", "[psr]")
{
    const double sr = 48000.0;
    const int n = static_cast<int>(sr * 8.0);
    // Dynamic: a quiet sine with a short full-scale click every second.
    // Squashed: a square-ish wave at the same peak - peak and loudness close.
    std::vector<float> dynamic(static_cast<size_t>(n)), squashed(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        const double t = i / sr;
        dynamic[static_cast<size_t>(i)] = static_cast<float>(0.05 * std::sin(2.0 * 3.14159265358979 * 440.0 * t));
        if (i % 48000 < 48) dynamic[static_cast<size_t>(i)] = 0.9f;
        const double s = std::sin(2.0 * 3.14159265358979 * 220.0 * t);
        squashed[static_cast<size_t>(i)] = static_cast<float>(0.9 * std::tanh(8.0 * s) / std::tanh(8.0));
    }
    const auto psrOf = [&](const std::vector<float>& x) {
        const auto tl = measureTrackTimeline({x.data()}, n, sr);
        const auto s = psr::series(tl.peakDbfs, tl.shortTermLufs, tl.hopSec);
        return psr::at(s, tl.hopSec, 6.0);
    };
    CHECK(psrOf(dynamic) > 15.0f);
    CHECK(psrOf(squashed) < psr::kSquashedBelowDb);
}
