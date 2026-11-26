// tests/ui/MultiLaneHeatmapTest.cpp
// VIS-1: the issue heatmap on the chart toolkit - its rows (the category
// axis), the findings-per-bucket density, the hover readout per row and the
// screen-reader summary built from the data.
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <utility>
#include "ui/Dashboard/MultiLaneHeatmap.h"

using namespace khensu;

namespace {

Recommendation finding(const char* what, float startSec, float endSec, float severity)
{
    Recommendation r;
    r.what = what;
    r.time_range = TimeRange(startSec, endSec);
    r.severity = severity;
    return r;
}

TimelineDataLane loudnessLane()
{
    TimelineDataLane dl;
    dl.label = "Loudness";
    dl.suffix = " LUFS";
    dl.minV = -20.0f;
    dl.maxV = 0.0f;
    dl.samples = {{0.0f, -14.0f}, {30.0f, -10.0f}, {60.0f, -8.0f}, {90.0f, -12.0f}};
    return dl;
}

} // namespace

TEST_CASE("VIS-1: an issue heatmap with no data shows the empty state", "[vis][chart][heatmap]")
{
    MultiLaneHeatmap h(nullptr);
    h.setBounds(0, 0, 600, 240);
    CHECK(h.isEmpty());
    CHECK(h.rowCount() == 0);
    CHECK(h.summary().contains("no timeline data"));

    h.setSpan(120.0f); // a span alone still has nothing to plot
    CHECK(h.isEmpty());
}

TEST_CASE("VIS-1: the heatmap's rows run top-down on one category axis", "[vis][chart][heatmap]")
{
    MultiLaneHeatmap h(nullptr);
    h.setBounds(0, 0, 600, 300);
    h.setSpan(120.0f);
    h.setDensityEnabled(true);
    h.setDataLanes({loudnessLane()});
    h.setLanes({{"Spectral", {finding("Harsh 3 kHz", 10.5f, 12.0f, 80.0f), finding("Muddy lows", 11.0f, 11.0f, 40.0f)}},
                {"Dynamics", {finding("Over-compressed chorus", 50.0f, 60.0f, 20.0f)}}});

    REQUIRE_FALSE(h.isEmpty());
    REQUIRE(h.rowCount() == 4);
    CHECK(h.rowName(0) == "Density");
    CHECK(h.rowName(1) == "Loudness");
    CHECK(h.rowName(2) == "Spectral");
    CHECK(h.rowName(3) == "Dynamics");
    CHECK(h.rowName(4).isEmpty());

    // The category axis lists them top-down, one equal band each, inside the plot.
    const auto plot = h.plotArea();
    for (int i = 0; i < h.rowCount(); ++i) {
        const auto a = h.rowArea(i);
        CHECK(a.getHeight() > 0.0f);
        CHECK(a.getY() >= plot.getY() - 0.5f);
        CHECK(a.getBottom() <= plot.getBottom() + 0.5f);
        if (i > 0) {
            CHECK(a.getY() > h.rowArea(i - 1).getY());
            CHECK(std::abs(a.getHeight() - h.rowArea(i - 1).getHeight()) < 1.0f);
        }
    }

    // A spectrogram adds its row on top.
    h.setSpectrogram(std::vector<std::vector<float>>(10, std::vector<float>(24, 0.5f)));
    REQUIRE(h.rowCount() == 5);
    CHECK(h.rowName(0) == "Spectrum");
    CHECK(h.rowName(1) == "Density");
}

TEST_CASE("VIS-1: findings map to density buckets and the hover reads each row", "[vis][chart][heatmap]")
{
    MultiLaneHeatmap h(nullptr);
    h.setBounds(0, 0, 600, 300);
    h.setSpan(120.0f);
    h.setDensityEnabled(true);
    h.setDataLanes({loudnessLane()});
    h.setLanes({{"Spectral", {finding("Harsh 3 kHz", 10.5f, 12.0f, 80.0f), finding("Muddy lows", 11.0f, 11.0f, 40.0f)}},
                {"Dynamics", {finding("Over-compressed chorus", 50.0f, 60.0f, 20.0f)}}});

    // 60 buckets over 120 s: 2 s each; both spectral findings start in 10-12 s.
    CHECK(h.densityBucketSeconds() == 2.0);

    h.setHover(std::make_pair(11.0, 0.0)); // the density row
    auto r = h.currentReadout();
    REQUIRE(r.lines.size() == 1);
    CHECK(r.title.startsWith("Density"));
    CHECK(r.lines[0].first.startsWith("2 findings"));

    h.setHover(std::make_pair(51.0, 0.0)); // one finding in 50-52 s
    r = h.currentReadout();
    REQUIRE(r.lines.size() == 1);
    CHECK(r.lines[0].first.startsWith("1 finding "));

    h.setHover(std::make_pair(100.0, 0.0)); // none
    CHECK(h.currentReadout().lines[0].first.startsWith("0 findings"));

    // The data row reads the nearest sample with its unit.
    h.setHover(std::make_pair(31.0, 1.0));
    r = h.currentReadout();
    REQUIRE(r.lines.size() == 1);
    CHECK(r.lines[0].first == "-10.0 LUFS");

    // A marker row lists the findings under the pointer, else says none.
    h.setHover(std::make_pair(11.0, 2.0));
    r = h.currentReadout();
    REQUIRE(r.lines.size() == 2);
    CHECK(r.lines[0].first == "Harsh 3 kHz");
    CHECK(r.lines[1].first == "Muddy lows");
    h.setHover(std::make_pair(90.0, 2.0));
    r = h.currentReadout();
    REQUIRE(r.lines.size() == 1);
    CHECK(r.lines[0].first == "No finding here");
    h.setHover(std::make_pair(55.0, 3.0)); // inside the dynamics finding's range
    r = h.currentReadout();
    REQUIRE(r.lines.size() == 1);
    CHECK(r.lines[0].first == "Over-compressed chorus");
}

TEST_CASE("VIS-1/VIS-8: the heatmap's screen-reader summary is built from its data", "[vis][chart][heatmap]")
{
    MultiLaneHeatmap h(nullptr);
    h.setBounds(0, 0, 600, 300);
    h.setSpan(120.0f);
    h.setDensityEnabled(true);
    h.setDataLanes({loudnessLane()});
    h.setLanes({{"Spectral", {finding("Harsh 3 kHz", 10.5f, 12.0f, 80.0f), finding("Muddy lows", 11.0f, 11.0f, 40.0f)}},
                {"Dynamics", {finding("Over-compressed chorus", 50.0f, 60.0f, 20.0f)}}});

    const auto s = h.summary();
    INFO(s.toStdString());
    CHECK(s.startsWith("Issue heatmap over"));
    CHECK(s.contains("Spectral 2"));
    CHECK(s.contains("Dynamics 1"));
    CHECK(s.contains("Loudness -14.0 LUFS to -8.0 LUFS"));
    CHECK(s.contains("most findings around"));
    CHECK_FALSE(s.contains("spectrogram"));

    h.setSpectrogram(std::vector<std::vector<float>>(10, std::vector<float>(24, 0.5f)));
    CHECK(h.summary().contains("with a spectrogram row"));

    // The spectrum row's hover names a frequency on the standard axis and a level.
    h.setHover(std::make_pair(30.0, 0.0));
    const auto r = h.currentReadout();
    REQUIRE(r.lines.size() == 2);
    CHECK(r.lines[0].first.startsWith("Around"));
    CHECK(r.lines[0].first.contains("Hz"));
    CHECK(r.lines[1].first.startsWith("Level 50 %"));
}
