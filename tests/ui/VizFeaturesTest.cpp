// tests/ui/VizFeaturesTest.cpp
// VIZ-3a / VIZ-4b / VIZ-5 / VIZ-N2 / VIZ-N5: the measured difference, the
// per-band width, the stem streamgraph and masking map, the album model and
// the visual report.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include "ui/ExportDelivery/AlbumConsistencyView.h"
#include "ui/ExportDelivery/VisualReport.h"
#include "ui/shared/FixDifferenceView.h"
#include "ui/shared/StemViz.h"
#include "ui/shared/StereoWidthBands.h"

using namespace khensu;
using Catch::Approx;

namespace {

/// A timeline with a flat spectrogram at `levelDb` (+ `tiltDb` above 1 kHz)
/// and steady loudness.
TrackTimeline flat(double seconds, float levelDb, float lufs, float tiltDb = 0.0f)
{
    TrackTimeline tl;
    tl.durationSec = seconds;
    tl.hopSec = 0.1;
    tl.sampleRate = 48000.0;
    const auto hops = static_cast<size_t>(seconds / 0.1);
    tl.shortTermLufs.assign(hops, lufs);
    tl.momentaryLufs.assign(hops, lufs);
    tl.peakDbfs.assign(hops, -6.0f);
    tl.bandHz = {100.0f, 300.0f, 1000.0f, 3000.0f, 10000.0f};
    tl.frameSec = seconds / 50.0;
    for (int f = 0; f < 50; ++f) {
        std::vector<float> row;
        for (const float hz : tl.bandHz)
            row.push_back(levelDb + (hz > 1000.0f ? tiltDb : 0.0f));
        tl.spectrogramDb.push_back(row);
    }
    return tl;
}

} // namespace

TEST_CASE("VIZ-3a: the measured difference spectrum and loudness curve", "[viz][difference]")
{
    const auto original = flat(10.0, -40.0f, -14.0f);
    const auto brighter = flat(10.0, -40.0f, -12.0f, 3.0f);
    const auto r = fix_difference::compute(original, brighter);
    REQUIRE(r.hz.size() == 5);
    CHECK(r.spectrumDb[0] == Approx(0.0).margin(1e-6));
    CHECK(r.spectrumDb[4] == Approx(3.0).margin(1e-4)); // the added top end
    REQUIRE_FALSE(r.loudnessDb.empty());
    CHECK(r.loudnessDb[50] == Approx(2.0).margin(1e-4)); // +2 LU throughout
}

TEST_CASE("VIZ-4b: stereo width per octave, from levels and from samples", "[viz][width]")
{
    std::vector<float> mid(10, -20.0f), side(10, -90.0f);
    side[5] = -20.0f; // 1 kHz as wide as it is loud
    const auto w = width_bands::fromOctaveLevels(mid, side);
    REQUIRE(w.size() == 10);
    CHECK(w[0] == Approx(0.0f).margin(1e-3));
    CHECK(w[5] == Approx(1.0f).margin(1e-3));

    // Identical channels are mono; independent noise is wide.
    std::vector<float> l(4096), r(4096), n(4096);
    juce::Random rng(7);
    for (size_t i = 0; i < l.size(); ++i) {
        l[i] = rng.nextFloat() * 2.0f - 1.0f;
        r[i] = l[i];
        n[i] = rng.nextFloat() * 2.0f - 1.0f;
    }
    const auto mono = width_bands::fromSamples(l.data(), r.data(), 4096, 48000.0);
    const auto wide = width_bands::fromSamples(l.data(), n.data(), 4096, 48000.0);
    REQUIRE(mono.size() == 10);
    CHECK(mono[6] < 0.01f);
    CHECK(wide[6] > 0.7f);
}

TEST_CASE("VIZ-5: the streamgraph and the masking map", "[viz][stems]")
{
    std::map<std::string, std::shared_ptr<const TrackTimeline>> stems;
    stems["vocals"] = std::make_shared<const TrackTimeline>(flat(8.0, -30.0f, -18.0f));
    stems["bass"] = std::make_shared<const TrackTimeline>(flat(8.0, -30.0f, -14.0f));
    const auto s = stem_viz::stream(stems);
    REQUIRE(s.stems.size() == 2);
    CHECK(s.stems[0] == "vocals"); // the fixed order: vocals, drums, bass, other
    CHECK(s.energy[1][4] > s.energy[0][4]); // the bass is louder

    // Identical spectra at the same level clash everywhere; a stem 30 dB
    // quieter in every band does not (the 20 dB gate).
    const auto same = stem_viz::masking("vocals", *stems["vocals"], "bass", *stems["bass"]);
    REQUIRE_FALSE(same.grid.empty());
    CHECK(same.worst > 0.0f);
    CHECK(same.grid[10][2] == Approx(1.0f));
    const auto quiet = flat(8.0, -60.0f, -40.0f);
    const auto apart = stem_viz::masking("vocals", *stems["vocals"], "other", quiet);
    CHECK(apart.worst == 0.0f);
}

TEST_CASE("VIZ-N2: the album model - median loudness and tonal deviation", "[viz][album]")
{
    std::vector<album_consistency::TrackData> tracks(3);
    tracks[0].lufs = -10.0f;
    tracks[1].lufs = -12.0f;
    tracks[2].lufs = -16.0f;
    tracks[0].bandDb = {0, 0, 0, 0};
    tracks[1].bandDb = {0, 0, 0, 0};
    tracks[2].bandDb = {0, 0, 6, 6}; // brighter than the rest
    const auto m = album_consistency::build(tracks, {100.0f, 1000.0f, 5000.0f, 10000.0f});
    REQUIRE(m.medianLufs.has_value());
    CHECK(*m.medianLufs == Approx(-12.0f));
    REQUIRE(m.deviationDb.size() == 3);
    CHECK(m.deviationDb[2][3] > 0.0);
    CHECK(m.deviationDb[0][3] < 0.0);

    // Integrated loudness from a steady -14 LUFS timeline is -14.
    const auto tl = flat(30.0, -40.0f, -14.0f);
    REQUIRE(album_consistency::integratedLufs(tl).has_value());
    CHECK(*album_consistency::integratedLufs(tl) == Approx(-14.0f).margin(0.01f));
}

TEST_CASE("VIZ-N5: the visual report is one self-contained page", "[viz][report]")
{
    juce::ScopedJuceInitialiser_GUI init;
    visual_report::Input in;
    in.trackName = "Song <1> & co";
    in.genreName = "Pop";
    in.generatedAt = "2026-11-24 10:00";
    in.scorecard.overall_score = 64.0f;
    in.scorecard.tier_label = "Needs Work";
    in.scorecard.tier_audience = "Capped by 1 severe issue - 86 without it";
    ScorecardCategory cat;
    cat.id = "tonal_balance_eq";
    cat.name = "Tonal Balance & EQ";
    cat.score = 60.0f;
    in.scorecard.categories.push_back(cat);
    in.scorecardSpread["tonal_balance_eq"] = {40.0f, 55.0f, 70.0f, 80.0f, 90.0f};
    for (float hz : {100.0f, 1000.0f, 10000.0f})
        in.hero.spectrum.push_back({hz, 0.0f});
    in.hero.gauges.resize(hero_spectrum::kGaugeCount);
    const auto tl = flat(20.0, -40.0f, -14.0f);
    in.timeline = &tl;
    in.findings.push_back({"Too quiet", "Raise the master", 70.0f, std::nullopt});
    const auto html = visual_report::buildHtml(in);
    CHECK(html.startsWith("<!doctype html>"));
    CHECK(html.contains("Song &lt;1&gt; &amp; co")); // escaped
    CHECK(html.contains("Capped by 1 severe issue"));
    CHECK(html.contains("<svg"));
    CHECK(html.contains("Loudness over time"));
    CHECK(html.contains("Too quiet"));
    CHECK_FALSE(html.contains("<script")); // no scripts
    CHECK_FALSE(html.contains("src=\"http")); // no external files
}

#include "ui/shared/ChangedRegions.h"

TEST_CASE("VIZ-3b: changed regions measured natively when the engine cannot answer", "[viz][regions]")
{
    const auto before = flat(20.0, -40.0f, -14.0f);
    auto after = flat(20.0, -40.0f, -14.0f);
    for (size_t i = 50; i < 80; ++i) after.shortTermLufs[i] = -11.0f; // 5.0 - 8.0 s louder
    const auto regions = changed_regions::fromTimelines(before, after);
    REQUIRE(regions.size() == 1);
    CHECK(regions[0].first == Approx(5.0).margin(0.11));
    CHECK(regions[0].second == Approx(8.0).margin(0.11));
    CHECK(changed_regions::fromTimelines(before, before).empty());
}
