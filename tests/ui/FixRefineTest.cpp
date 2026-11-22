// tests/ui/FixRefineTest.cpp
// NEW-4: Stronger / Gentler scale a finding's move; "Hear what changed"
// reads the changed regions and finds the version a fix was rendered from.
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "../../desktop-app/Source/session/RecommendationChain.h"
#include "../../desktop-app/Source/ui/shared/ChangedRegions.h"

using namespace khensu;
using Catch::Approx;

TEST_CASE("scaledRecommendation halves gains, keeps levels and frequencies", "[issues][new4]")
{
    Recommendation rec;
    rec.suggested_parameters = {{"eq_gain_db", -4.0f},  {"eq_freq_hz", 3200.0f},  {"comp_ratio", 3.0f},
                                {"comp_threshold_db", -18.0f}, {"stereo_width", 1.4f}, {"reduce_gain_db", 2.0f}};
    const auto half = scaledRecommendation(rec, 0.5f);
    const auto& p = half.suggested_parameters;
    CHECK(p.at("eq_gain_db") == Approx(-2.0f));
    CHECK(p.at("eq_freq_hz") == Approx(3200.0f));
    CHECK(p.at("comp_ratio") == Approx(2.0f));
    CHECK(p.at("comp_threshold_db") == Approx(-18.0f));
    CHECK(p.at("stereo_width") == Approx(1.2f));
    CHECK(p.at("reduce_gain_db") == Approx(1.0f));
    // Full size is the original move.
    CHECK(scaledRecommendation(rec, 1.0f).suggested_parameters == rec.suggested_parameters);
}

TEST_CASE("changed regions parse and format", "[issues][new4]")
{
    auto v = juce::JSON::parse(R"({"comparable": true, "reason": "", "method": "rms", "duration_sec": 200,
        "changed_sec": 12, "changed_fraction": 0.06,
        "regions": [{"start_sec": 30.5, "end_sec": 38.0}, {"start_sec": 90, "end_sec": 90}]})");
    const auto r = changed_regions::fromResponse(v);
    REQUIRE(r.size() == 1); // an empty region is dropped
    CHECK(r[0].first == Approx(30.5));
    CHECK(changed_regions::clock(r[0].first) == "0:31");
    CHECK(changed_regions::clock(125.0) == "2:05");
    v.getDynamicObject()->setProperty("comparable", false);
    CHECK(changed_regions::fromResponse(v).empty());
}

#include "../../desktop-app/Source/ui/shared/ScanSummary.h"

TEST_CASE("scan summaries keep scalars and short lists, drop grids (WF-14)", "[scans][wf14]")
{
    auto v = juce::JSON::parse(R"({"is_stereo": true, "note": "ok",
        "systems": [{"name": "phone", "score": 42, "worst_band_hz": [60, 120]}],
        "grid": [[1,2,3],[4,5,6],[1,2,3],[4,5,6],[1,2,3],[4,5,6],[1,2,3],[4,5,6],[1,2,3],[4,5,6],[1,2,3],[4,5,6],[1,2,3]]})");
    const auto c = khensu::scan_summary::compact(v);
    CHECK((bool)c["is_stereo"]);
    CHECK(c["systems"][0]["score"].toString() == "42");
    CHECK(c["systems"][0]["worst_band_hz"].size() == 2);
    CHECK(c["grid"].isVoid()); // 13 rows: a grid, not a summary
}

#include "../../desktop-app/Source/ui/Settings/ComponentsPanel.h"

TEST_CASE("Components list parses (FEAT-10)", "[components][feat10]")
{
    auto v = juce::JSON::parse(R"({"components": [
        {"id": "asr", "name": "Lyrics transcription", "description": "d", "installed": false, "installable": true},
        {"id": "stems", "name": "Demucs", "description": "d", "installed": false, "installable": false,
         "how": "pwsh packaging/install-optional-stack.ps1"}]})");
    const auto rows = khensu::ComponentsPanel::parse(v);
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].installable);
    CHECK_FALSE(rows[1].installable);
    CHECK(rows[1].how.contains("install-optional-stack"));
}

#include "../../desktop-app/Source/ui/Dashboard/GenreFitPanel.h"

TEST_CASE("Genre fit rows come from the measured stats (NEW-3)", "[genrefit][new3]")
{
    auto stats = juce::JSON::parse(R"({"genres": {"trap": {"n": 10,
        "lufs": {"median": -8.7, "p10": -11.0, "p90": -7.0},
        "width": {"median": 0.2, "p10": 0.05, "p90": 0.3}}}})");
    khensu::AnalysisResult r;
    r.loudness.lufs_integrated = -9.5f;
    r.stereo.width = 0.4f;
    int n = 0;
    const auto rows = khensu::GenreFitPanel::rowsFor(stats, "trap", r, n);
    CHECK(n == 10);
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].median == Approx(-8.7));
    REQUIRE(rows[1].value.has_value());
    CHECK(*rows[1].value == Approx(0.4));
    CHECK(khensu::GenreFitPanel::rowsFor(stats, "jazz", r, n).empty());
    CHECK(n == 0);
    CHECK(khensu::GenreFitPanel::position(5.0, 0.0, 10.0) == Approx(0.5));
    CHECK(khensu::GenreFitPanel::position(20.0, 0.0, 10.0) == Approx(1.0));
}

#include "../../desktop-app/Source/ui/shared/FindingEvidence.h"

TEST_CASE("Finding evidence parses and words the measured rates (NEW-2)", "[evidence][new2]")
{
    auto doc = juce::JSON::parse(R"({"tracks": 1140, "findings": {
        "low_end_heavy": {"false_alarm_rate": 0.083, "fired_on": 95,
                          "detects": [{"defect": "low_shelf_+10", "rate": 0.62, "tracks": 380}]},
        "dc_offset": {"false_alarm_rate": 0.0, "fired_on": 0}}})");
    const auto t = khensu::finding_evidence::parse(doc);
    REQUIRE(t.count("low_end_heavy") == 1);
    const auto& e = t.at("low_end_heavy");
    CHECK(e.corpusTracks == 1140);
    REQUIRE(e.detects.size() == 1);
    const auto text = khensu::finding_evidence::describe(e);
    CHECK(text.contains("8.3%"));
    CHECK(text.contains("1140"));
    CHECK(text.contains("low shelf +10"));
    CHECK(text.contains("62%"));
    CHECK(khensu::finding_evidence::describe(t.at("dc_offset")).contains("0.0%"));

    auto withFix = juce::JSON::parse(R"({"tracks": 10, "findings": {
        "low_end_light": {"false_alarm_rate": 0.1, "fired_on": 1, "fix": {"tried": 20, "clears_rate": 0.85}},
        "clipping_detected": {"false_alarm_rate": 0.01, "fired_on": 0, "fix": {"tried": 0, "advice_only": true}}}})");
    const auto t2 = khensu::finding_evidence::parse(withFix);
    CHECK(khensu::finding_evidence::describe(t2.at("low_end_light")).contains("cleared it on 85% of 20"));
    CHECK(khensu::finding_evidence::describe(t2.at("clipping_detected")).contains("advice only"));
}

TEST_CASE("Finding evidence words alarms, spec checks and per-finding counts (EVID-1/2, FULL-1)", "[evidence]")
{
    auto doc = juce::JSON::parse(R"({"tracks": 1462, "findings": {
        "lufs_too_quiet": {"false_alarm_rate": 0.384, "alarm_rate": 0.12, "fired_on": 561},
        "true_peak_over": {"false_alarm_rate": 0.824, "alarm_rate": 0.5, "fired_on": 1205, "kind": "spec"},
        "resonance_hotspot": {"false_alarm_rate": 0.05, "alarm_rate": 0.05, "fired_on": 10, "tracks": 200,
                              "source": "full"}}})");
    const auto t = khensu::finding_evidence::parse(doc);
    const auto quiet = khensu::finding_evidence::describe(t.at("lufs_too_quiet"));
    CHECK(quiet.contains("38.4%"));
    CHECK(quiet.contains("12.0% at moderate or worse"));
    const auto peak = khensu::finding_evidence::describe(t.at("true_peak_over"));
    CHECK(peak.contains("delivery-spec check"));
    CHECK_FALSE(peak.contains("graded against their own genre"));
    CHECK(t.at("resonance_hotspot").corpusTracks == 200);
    // Every firing an alarm: no "minor notes" clause.
    CHECK_FALSE(khensu::finding_evidence::describe(t.at("resonance_hotspot")).contains("minor"));
}
