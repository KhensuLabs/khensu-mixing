// tests/shared-core/test_viz_foundation.cpp
// VIZ-F1 / VIZ-1 / VIZ-P3: the chart toolkit's axes, the measured genre
// spread, the shared band-power definition and the readiness cap.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <khensu/analysis/BandPower.h>
#include <khensu/recommendations/MixScorecard.h>
#include <khensu/targets/GenreSpread.h>
#include <khensu/ui/ChartKit.h>

using namespace khensu;
using Catch::Approx;
namespace chart = khensu::ui::chart;

TEST_CASE("VIZ-F1: linear and log axes map and invert", "[viz][chart]")
{
    chart::Axis lin;
    lin.lo = -30.0;
    lin.hi = 0.0;
    CHECK(lin.norm(-15.0) == Approx(0.5));
    CHECK(lin.denorm(0.25) == Approx(-22.5));

    chart::Axis log;
    log.lo = 20.0;
    log.hi = 20000.0;
    log.scale = chart::Scale::Log;
    CHECK(log.norm(632.456) == Approx(0.5).margin(1e-3));
    CHECK(log.denorm(log.norm(1000.0)) == Approx(1000.0));
}

TEST_CASE("VIZ-F1: ticks are round numbers inside the range", "[viz][chart]")
{
    chart::Axis a;
    a.lo = -36.0;
    a.hi = 12.0;
    const auto t = a.ticks(6);
    REQUIRE(t.size() >= 3);
    for (const double v : t) {
        CHECK(v >= a.lo);
        CHECK(v <= a.hi);
        CHECK(std::fmod(std::abs(v), 5.0) == Approx(0.0).margin(1e-9));
    }
    chart::Axis f;
    f.lo = 20.0;
    f.hi = 20000.0;
    f.scale = chart::Scale::Log;
    f.format = chart::Format::Hz;
    const auto ft = f.ticks(12);
    CHECK(std::find(ft.begin(), ft.end(), 1000.0) != ft.end());
    CHECK(f.label(1000.0) == "1k");
    CHECK(f.label(1500.0, true) == "1.5 kHz");
    CHECK(f.label(80.0, true) == "80 Hz");
}

TEST_CASE("VIZ-F1: time and unit labels", "[viz][chart]")
{
    chart::Axis t;
    t.format = chart::Format::Time;
    CHECK(t.label(150.0) == "2:30");
    CHECK(t.label(150.4, true) == "2:30.4");
    chart::Axis d;
    d.unit = "LUFS";
    d.decimals = 1;
    CHECK(d.label(-9.84, true) == "-9.8 LUFS");
    t.lo = 0.0;
    t.hi = 240.0;
    for (const double v : t.ticks(6))
        CHECK(std::fmod(v, 30.0) == Approx(0.0).margin(1e-9));
}

TEST_CASE("VIZ-1: band power on the shared grid, interpolated", "[viz][bandpower]")
{
    const auto grid = band_power::bandCentres();
    REQUIRE(grid.front() == Approx(25.0f));
    CHECK(grid[6] == Approx(50.0f).epsilon(1e-4));
    std::vector<band_power::Point> pts{{100.0f, 0.0f}, {400.0f, -6.0f}};
    CHECK(band_power::at(pts, 200.0f) == Approx(-3.0f));
    CHECK(band_power::at(pts, 50.0f) == Approx(0.0f));
    CHECK(band_power::at(pts, 1000.0f) == Approx(-6.0f));
}

TEST_CASE("VIZ-1: the genre spread parses, falls back and ranks", "[viz][spread]")
{
    const std::string json = R"({"version":1,"band_hz":[100,200,400,800],
        "genres":{"glitch-hop":{"n":8,
            "band_db":{"p10":[-4,-3,-2,null],"p25":[-3,-2,-1,null],"p50":[-2,-1,0,null],"p75":[-1,0,1,null],"p90":[0,1,2,null]},
            "metrics":{"lufs":[-12,-11,-10,-9,-8,-8,-7,-6]},
            "scorecard":{"tonal_balance_eq":{"p10":40,"p25":55,"p50":70,"p75":80,"p90":90}}}},
        "all":{"n":100,"metrics":{"lufs":[-20,-10]}}})";
    const auto s = GenreSpread::fromJson(json);
    REQUIRE(s.bandHz.size() == 4);
    const auto* g = s.find("glitch-hop");
    REQUIRE(g != nullptr);
    CHECK(g->tracks == 8);
    REQUIRE(g->bandDb.size() == 4);
    REQUIRE(g->bandDb[0].has_value());
    CHECK((*g->bandDb[2])[GenreSpread::kP90] == Approx(2.0f));
    CHECK_FALSE(g->bandDb[3].has_value());
    // -8.5 LUFS: 4 of 8 released tracks are quieter.
    CHECK(*g->fractionBelow("lufs", -8.5f) == Approx(0.5f));
    // A tie counts half: -8 sits between two -8s.
    CHECK(*g->fractionBelow("lufs", -8.0f) == Approx(5.0f / 8.0f));
    CHECK_FALSE(g->fractionBelow("width", 0.5f).has_value());
    CHECK(g->scorecard.at("tonal_balance_eq")[GenreSpread::kP50] == Approx(70.0f));
    // An unknown genre falls back to the whole corpus.
    const auto* all = s.find("polka");
    REQUIRE(all != nullptr);
    CHECK(all->tracks == 100);
    CHECK(GenreSpread::fromJson("not json").empty());
}

namespace {
Recommendation rec(float severity, RecommendationType type = RecommendationType::Technical)
{
    Recommendation r;
    r.severity = severity;
    r.type = type;
    return r;
}
} // namespace

TEST_CASE("VIZ-P3: a severe finding caps readiness and says why", "[viz][scorecard]")
{
    AnalysisResult a;
    GenreEntry g;
    const auto clean = buildMixScorecard(a, g, {});
    CHECK_FALSE(clean.capped());

    const auto severe = buildMixScorecard(a, g, {rec(80.0f), rec(70.0f), rec(10.0f)});
    CHECK(severe.severe_open == 2);
    CHECK(severe.overall_score <= MixScorecard::kSevereCap);
    CHECK(severe.uncapped_score == Approx(clean.uncapped_score));
    if (severe.capped()) {
        CHECK(severe.tier_label != std::string(MixScorecard::kTierHighlySkilled));
        CHECK(severe.tier_audience.find("Capped by 2 severe issues") != std::string::npos);
    }

    const auto moderate = buildMixScorecard(a, g, {rec(40.0f)});
    CHECK(moderate.overall_score <= MixScorecard::kModerateCap);
    CHECK(moderate.tier_label != std::string(MixScorecard::kTierExceptional));

    // Strengths never cap.
    const auto strength = buildMixScorecard(a, g, {rec(90.0f, RecommendationType::Strength)});
    CHECK(strength.severe_open == 0);
    CHECK(strength.overall_score == Approx(clean.overall_score));
}

#include <khensu/analysis/TrackTimeline.h>

TEST_CASE("VIZ-2: the native timeline measures loudness, true-peak events and the spectrogram", "[viz][timeline]")
{
    const double sr = 48000.0;
    const int n = static_cast<int>(sr * 8.0);
    std::vector<float> l(static_cast<size_t>(n)), r(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        // 997 Hz at -6 dBFS for 6 s, then 1 s near full scale, then silence.
        const double t = i / sr;
        const float amp = t < 6.0 ? 0.5f : t < 7.0 ? 0.999f : 0.0f;
        l[static_cast<size_t>(i)] = r[static_cast<size_t>(i)] =
            amp * static_cast<float>(std::sin(2.0 * juce::MathConstants<double>::pi * 997.0 * t + 0.3));
    }
    const auto tl = measureTrackTimeline({l.data(), r.data()}, n, sr);
    REQUIRE_FALSE(tl.empty());
    CHECK(tl.durationSec == Approx(8.0));
    // Stereo -6 dBFS sine: about -6 LUFS once the 3 s window is full.
    CHECK(TrackTimeline::at(tl.shortTermLufs, tl.hopSec, 4.0) == Approx(-6.0f).margin(0.4f));
    CHECK(TrackTimeline::at(tl.momentaryLufs, tl.hopSec, 7.9) < -40.0f); // the silence
    // The near-full-scale second crosses -1 dBTP; the -6 dBFS part never does.
    REQUIRE_FALSE(tl.truePeakEvents.empty());
    CHECK(tl.truePeakEvents.front().startSec >= 5.95);
    CHECK(tl.truePeakEvents.back().endSec <= 7.05);
    CHECK(tl.maxTruePeakDbtp > -0.2f);
    // The spectrogram's loudest band is the one holding 997 Hz.
    REQUIRE_FALSE(tl.spectrogramDb.empty());
    const auto& frame = tl.spectrogramDb[tl.spectrogramDb.size() / 3];
    const auto loudest = static_cast<size_t>(std::max_element(frame.begin(), frame.end()) - frame.begin());
    CHECK(tl.bandHz[loudest] == Approx(997.0f).epsilon(0.1));
}

#include <khensu/dsp/DemoSong.h>

TEST_CASE("VIZ-U2 / VIZ-F3: the demo song is full length, deterministic and has things to find", "[viz][demo]")
{
    CHECK(demo_song::kDurationSec == Approx(224.0));
    REQUIRE(demo_song::arrangement().size() == 9);
    CHECK(demo_song::arrangement().back().firstBar + demo_song::arrangement().back().bars == demo_song::kBars);
    const auto a = demo_song::render(22050.0);
    const auto b = demo_song::render(22050.0);
    REQUIRE(a.getNumSamples() == static_cast<int>(demo_song::kDurationSec * 22050.0));
    REQUIRE(a.getNumChannels() == 2);
    // Deterministic: the same samples every time.
    for (int i = 0; i < a.getNumSamples(); i += 997)
        REQUIRE(a.getSample(0, i) == b.getSample(0, i));
    // Not silent, not clipped hard, wider than mono (the pads).
    CHECK(a.getMagnitude(0, a.getNumSamples()) > 0.5f);
    CHECK(a.getMagnitude(0, a.getNumSamples()) <= 1.0f);
    double side = 0.0, mid = 0.0;
    for (int i = 0; i < a.getNumSamples(); ++i) {
        const double l = a.getSample(0, i), r = a.getSample(1, i);
        mid += (l + r) * (l + r);
        side += (l - r) * (l - r);
    }
    CHECK(side > 0.001 * mid);
}
