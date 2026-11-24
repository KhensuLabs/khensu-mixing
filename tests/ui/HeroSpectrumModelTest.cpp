// tests/ui/HeroSpectrumModelTest.cpp
// UI-1: the hero spectrum's model (band power, the 1 kHz normalisation, how
// each finding is drawn, the fix's expected response, the gauges) and its
// hit testing; OVH-6: the fix prediction from finding_evidence.json.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include "ui/shared/FindingEvidence.h"
#include "ui/shared/HeroSpectrumModel.h"
#include "ui/shared/HeroSpectrumView.h"

namespace hs = khensu::hero_spectrum;
using Catch::Matchers::WithinAbs;
using khensu::IIssues;
using khensu::Recommendation;

namespace {
IIssues::Issue issueFor(const std::string& code, float severity)
{
    IIssues::Issue i;
    i.recId = code + "_f1";
    i.code = code;
    i.rec.id = i.recId;
    i.rec.what = code;
    i.rec.severity = severity;
    return i;
}

khensu::AnalysisResult flatResult()
{
    khensu::AnalysisResult r;
    r.sample_rate = 48000.0;
    r.loudness.lufs_integrated = -9.0f;
    r.spectral.rms_per_octave_db = std::vector<float>(10, -30.0f);
    return r;
}
} // namespace

TEST_CASE("Hero spectrum: band power is flat for pink noise and normalised at 1 kHz", "[ui1]")
{
    // Pink noise: the octave DENSITY falls 3 dB/octave, its band power is flat.
    std::vector<float> octaves;
    for (const double fc : {31.5, 63.0, 125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0, 16000.0})
        octaves.push_back(static_cast<float>(-20.0 - 10.0 * std::log10(fc)));
    auto points = hs::bandPowerFromOctaves(octaves);
    REQUIRE(points.size() == 10);
    hs::normaliseAt1k(points);
    for (const auto& p : points)
        CHECK_THAT(p.db, WithinAbs(0.0, 0.05));

    // White noise per FFT bin: band power rises ~3 dB per octave.
    std::vector<float> fft(2048, -60.0f);
    auto bands = hs::bandPowerFromFft(fft, 48000.0);
    REQUIRE(bands.size() > 40);
    hs::normaliseAt1k(bands);
    const auto at = [&bands](float hz) {
        const hs::Point* best = &bands.front();
        for (const auto& p : bands)
            if (std::abs(std::log2(p.hz / hz)) < std::abs(std::log2(best->hz / hz))) best = &p;
        return best->db;
    };
    CHECK_THAT(at(4000.0f) - at(1000.0f), WithinAbs(6.02, 0.6));
    CHECK_THAT(at(1000.0f), WithinAbs(0.0, 0.8));
}

TEST_CASE("Hero spectrum: each finding is drawn where it lives", "[ui1]")
{
    float lo = 0, hi = 0, hz = 0;
    Recommendation rec;
    CHECK(hs::shapeFor("tilt_too_dark", rec, lo, hi, hz) == hs::Shape::Tilt);
    CHECK(hs::shapeFor("low_end_light", rec, lo, hi, hz) == hs::Shape::Band);
    CHECK(lo == 20.0f);
    CHECK(hi == 177.0f);
    rec.freq_range = khensu::FrequencyRange{2500.0f, 5000.0f};
    CHECK(hs::shapeFor("harshness_detected", rec, lo, hi, hz) == hs::Shape::Band);
    CHECK(lo == 2500.0f); // the finding's own range wins
    Recommendation res;
    res.suggested_parameters["eq_center_hz"] = 3150.0f;
    CHECK(hs::shapeFor("resonance_hotspot", res, lo, hi, hz) == hs::Shape::Marker);
    CHECK(hz == 3150.0f);
    CHECK(hs::shapeFor("lufs_too_quiet", Recommendation{}, lo, hi, hz) == hs::Shape::Chip);
    CHECK(hs::gaugeFor("lufs_too_quiet") == hs::kLoudness);
    CHECK(hs::gaugeFor("true_peak_over") == hs::kTruePeak);
    CHECK(hs::gaugeFor("over_compressed") == hs::kCrest);
    CHECK(hs::gaugeFor("stereo_too_wide") == hs::kWidth);
    CHECK(hs::gaugeFor("dc_offset") == -1);
    CHECK(hs::chipLabelFor("dc_offset") == "DC offset");
}

TEST_CASE("Hero spectrum: a fix's expected response and the check's shrink", "[ui1]")
{
    khensu::ipc::HttpServiceBridge::EffectSpec shelf;
    shelf.effect = "LowShelfFilter";
    shelf.parameters["cutoff_frequency_hz"] = 100.0f;
    shelf.parameters["gain_db"] = 6.0f;
    shelf.parameters["q"] = 0.707f;
    khensu::ipc::HttpServiceBridge::EffectSpec gain;
    gain.effect = "Gain";
    gain.parameters["gain_db"] = -2.0f;
    CHECK_THAT(hs::chainResponseDb({shelf}, 25.0f, 48000.0), WithinAbs(6.0, 0.5));
    CHECK_THAT(hs::chainResponseDb({shelf}, 8000.0f, 48000.0), WithinAbs(0.0, 0.1));
    CHECK_THAT(hs::chainResponseDb({shelf, gain}, 8000.0f, 48000.0), WithinAbs(-2.0, 0.1));

    // Half the fix: half the shelf.
    Recommendation rec;
    rec.suggested_parameters = {{"eq_type", 2.0f}, {"eq_freq_hz", 100.0f}, {"eq_gain_db", 6.0f}};
    const std::vector<hs::Point> at{{25.0f, 0.0f}, {8000.0f, 0.0f}};
    const auto full = hs::fixResponse(rec, 1.0f, at, 48000.0);
    const auto half = hs::fixResponse(rec, 0.5f, at, 48000.0);
    CHECK_THAT(half[0], WithinAbs(full[0] * 0.5, 0.3));
    CHECK(hs::fixResponse(rec, 0.0f, at, 48000.0)[0] == 0.0f);

    const auto r = flatResult();
    auto low = issueFor("low_end_light", 70.0f);
    auto loud = issueFor("lufs_too_loud", 40.0f);
    auto dark = issueFor("tilt_too_dark", 50.0f);
    dark.check = IIssues::Check::Resolved;
    khensu::fix_verification::Result check;
    khensu::fix_verification::FindingCheck c;
    c.code = "low_end_light";
    c.severityBefore = 70.0f;
    c.severityAfter = 35.0f;
    check.findings.push_back(c);
    khensu::GenreEntry genre;
    genre.id = "pop";
    genre.name = "Pop";
    genre.lufs_range_min = -14.0f;
    genre.lufs_range_max = -10.0f;
    const auto m = hs::build(r, genre, {low, loud, dark}, &check);
    REQUIRE(m.findings.size() == 3);
    CHECK(m.hasSpectrum());
    CHECK(m.findings[0].shape == hs::Shape::Band);
    CHECK_THAT(m.findings[0].remaining, WithinAbs(0.5, 1e-4));
    CHECK(m.findings[1].gauge == hs::kLoudness);
    CHECK(m.findings[2].remaining == 0.0f); // resolved: drawn as a faint outline
    REQUIRE(m.tiltTarget.has_value());
    CHECK(m.genreName == "Pop");
    REQUIRE(m.gauges.size() == static_cast<size_t>(hs::kGaugeCount));
    CHECK(m.gauges[hs::kLoudness].value == "-9.0 LUFS");
    CHECK(*m.gauges[hs::kLoudness].windowHi == -10.0f);
    CHECK(hs::gaugeSeverity(m, hs::kLoudness) == 40.0f);
}

TEST_CASE("Hero spectrum view: hover and click find the finding under the pointer", "[ui1]")
{
    juce::ScopedJuceInitialiser_GUI init;
    khensu::HeroSpectrumView view;
    view.setSize(1000, 380);
    const auto r = flatResult();
    auto low = issueFor("low_end_light", 70.0f);
    auto loud = issueFor("lufs_too_loud", 40.0f);
    view.setModel(hs::build(r, std::nullopt, {low, loud}, nullptr), {}, false);
    const auto plot = view.plotArea();
    REQUIRE_FALSE(plot.isEmpty());
    CHECK(view.findingAt({view.xForHz(60.0f), plot.getCentreY()}) == "low_end_light_f1");
    CHECK(view.findingAt({view.xForHz(3000.0f), plot.getCentreY()}).empty());
    // VIZ-P2: the tiles are measurements (a click explains the metric); the
    // loudness finding is a chip in the "Top findings" row beneath them.
    const auto tiles = view.tilesArea();
    CHECK(view.tileAt({tiles.getX() + 20.0f, tiles.getCentreY()}) == hs::kLoudness);
    CHECK(view.findingAt({tiles.getX() + 20.0f, tiles.getCentreY()}).empty());
    const auto row = view.findingsArea();
    CHECK(view.findingAt({row.getX() + 110.0f, row.getCentreY()}) == "lufs_too_loud_f1");
    CHECK(row.getY() >= tiles.getBottom());

    std::string clicked;
    view.onFindingClicked = [&clicked](const std::string& key) { clicked = key; };
    const juce::Point<float> at(view.xForHz(60.0f), plot.getCentreY());
    view.mouseUp(juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), at, {}, 1.0f, 0.0f, 0.0f, 0.0f,
                                  0.0f, &view, &view, juce::Time::getCurrentTime(), at, juce::Time::getCurrentTime(),
                                  1, false));
    CHECK(clicked == "low_end_light_f1");
    CHECK(view.selected() == "low_end_light_f1");

    // Paints with data, with an expected after-curve, and without data.
    juce::Image img(juce::Image::ARGB, 1000, 380, true);
    juce::Graphics g(img);
    view.paintEntireComponent(g, false);
    auto model = hs::build(r, std::nullopt, {low}, nullptr);
    std::vector<float> after(model.spectrum.size(), 1.0f);
    view.setModel(std::move(model), after, false);
    CHECK_FALSE(view.isAnimating()); // not showing: snaps
    view.paintEntireComponent(g, false);
    view.setModel({}, {}, false);
    view.paintEntireComponent(g, false);
}

TEST_CASE("Fix prediction reads the evidence table, side effects optional (OVH-6)", "[ovh6]")
{
    namespace fe = khensu::finding_evidence;
    const auto doc = juce::JSON::parse(R"JSON({"tracks": 200, "findings": {
        "low_end_light": {"false_alarm_rate": 0.1, "fired_on": 20,
            "fix": {"tried": 100, "advice_only": false, "clears_rate": 0.78,
                    "side_effects": [{"code": "low_end_heavy", "rate": 0.04, "tracks": 100},
                                     {"code": "true_peak_over", "rate": 0.12, "tracks": 100}]}},
        "tilt_too_dark": {"false_alarm_rate": 0.05, "fired_on": 10,
            "fix": {"tried": 3, "advice_only": false, "clears_rate": 1.0}},
        "lra_too_dynamic": {"false_alarm_rate": 0.2, "fired_on": 40, "fix": {"tried": 0, "advice_only": true}},
        "hook_density_low": {"false_alarm_rate": 0.3, "fired_on": 60}}})JSON");
    const auto table = fe::parse(doc);
    REQUIRE(table.count("low_end_light") == 1);
    const auto& low = table.at("low_end_light");
    REQUIRE(low.sideEffects.size() == 2);
    CHECK(low.sideEffects[0].code == "true_peak_over"); // most frequent first
    const auto text = fe::predictFix(low);
    CHECK(text.startsWith("Fix clears this on 78% of 100 released tracks tested"));
    CHECK(text.contains("may introduce: True peak over (12%), Low end heavy (4%)"));
    CHECK_FALSE(fe::predictFix(table.at("tilt_too_dark")).contains("may introduce")); // absent: tolerated
    CHECK(fe::predictFix(table.at("lra_too_dynamic")).startsWith("Advice only"));
    CHECK(fe::predictFix(table.at("hook_density_low")).isEmpty());
}

TEST_CASE("Hero model: released tracks' spread, dots and captions (VIZ-1a/1b, VIZ-P6)", "[viz]")
{
    const auto spread = khensu::GenreSpread::fromJson(R"({"band_hz":[100,1000,10000],
        "genres":{"pop":{"n":10,
            "band_db":{"p10":[-2,-2,-12],"p25":[0,-1,-10],"p50":[2,0,-8],"p75":[4,1,-6],"p90":[6,2,-4]},
            "metrics":{"lufs":[-14,-13,-12,-11,-10,-9,-8,-8,-7,-6],"lra":[2,3,4,5,6,7,8,9,10,11]}}}})");
    khensu::GenreEntry genre;
    genre.id = "pop";
    genre.name = "Pop";
    auto r = flatResult();
    r.loudness.lufs_integrated = -7.5f;
    r.loudness.lra_measured = false; // a short file: undefined, not bad
    const auto m = hs::build(r, genre, {}, nullptr, &spread);
    REQUIRE(m.hasSpread() == false); // three bands are too few to draw
    CHECK(m.spreadTracks == 10);
    CHECK(m.spreadName == "Pop");
    const auto& loud = m.gauges[hs::kLoudness];
    REQUIRE(loud.fractionBelow.has_value());
    CHECK(loud.caption == "Louder than 80 % of released Pop");
    CHECK(loud.released.size() == 10);
    const auto& lra = m.gauges[hs::kLra];
    CHECK_FALSE(lra.defined);
    CHECK(lra.value == "-");
    CHECK(lra.caption.isEmpty());
    CHECK(lra.released.size() == 10); // the dots still show where released tracks sit

    // strayAt: 0 inside the middle 50 %, +/-1 at the edge of the middle 80 %.
    hs::Model sm;
    for (float hz : {100.0f, 200.0f, 400.0f, 800.0f})
        sm.spread.push_back({hz, {-4.0f, -2.0f, 0.0f, 2.0f, 4.0f}});
    CHECK(hs::strayAt(sm, 200.0f, 1.0f) == 0.0f);
    CHECK(hs::strayAt(sm, 200.0f, 4.0f) == 1.0f);
    CHECK(hs::strayAt(sm, 200.0f, -3.0f) == -0.5f);
    CHECK(hs::strayAt(sm, 5000.0f, 9.0f) == 0.0f); // outside the measured range
}
