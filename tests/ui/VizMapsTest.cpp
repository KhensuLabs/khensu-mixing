// tests/ui/VizMapsTest.cpp - the 2026-11-25 visualisation items:
//   VIS-2  one spectrogram map and one diverging map;
//   VIS-3  one frequency axis (20 Hz - 20 kHz) with ChartKit's labels, heat
//          maps placed on it by their band edges;
//   VIS-6  meters drawn greyed while they show analysed values;
//   VIS-7  reduced motion (persisted; ValueAnimator jumps);
//   BUG-5  the Mix / Master meter card lays its meters out without overlap.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "support/FakeAppController.h"
#include "support/ScopedTempDir.h"
#include "ui/shared/HeatmapImage.h"
#include "ui/shared/MeterBridge.h"
#include "ui/shared/ReducedMotion.h"
#include "ui/shared/SpectrogramImage.h"
#include "ui/shared/ValueAnimator.h"
#include "ui/shared/VizMaps.h"
#include "settings/OnlineSettings.h"
#include <khensu/ui/StudioMeters.h>
#include <khensu/ui/VizFrequencyAxis.h>
#include <khensu/ui/VizPalette.h>

using namespace khensu;
using Catch::Matchers::WithinAbs;

TEST_CASE("VIS-3: one frequency axis with ChartKit's tick labels", "[viz][vis3]")
{
    CHECK(ui::viz::kFreqMinHz == 20.0f);
    CHECK(ui::viz::kFreqMaxHz == 20000.0f);
    CHECK(ui::viz::freqTickLabel(20.0f) == "20");
    CHECK(ui::viz::freqTickLabel(1000.0f) == "1k");
    CHECK(ui::viz::freqTickLabel(20000.0f) == "20k");
    const auto axis = ui::viz::frequencyAxis();
    for (const float hz : ui::viz::kFreqTicksHz)
        CHECK(ui::viz::freqTickLabel(hz) == axis.label(hz));
    CHECK_THAT(ui::viz::freqNorm(20.0f), WithinAbs(0.0, 1e-6));
    CHECK_THAT(ui::viz::freqNorm(20000.0f), WithinAbs(1.0, 1e-6));
    CHECK_THAT(ui::viz::freqAt(ui::viz::freqNorm(440.0f)), WithinAbs(440.0, 0.05));
    const juce::Rectangle<float> plot(0.0f, 0.0f, 100.0f, 300.0f);
    CHECK_THAT(ui::viz::hzForY(plot, ui::viz::yForHz(plot, 2500.0f)), WithinAbs(2500.0, 0.5));
}

TEST_CASE("VIS-3: a heat map's bands sit on the axis by their edges", "[viz][vis3]")
{
    // Two uneven bands: 20-100 Hz and 100 Hz - 20 kHz, then a band that
    // stops at 10 kHz (the strip above is not measured).
    const std::vector<float> edges{20.0f, 100.0f, 10000.0f};
    CHECK(bandAtHz(edges, 2, 50.0f) == 0);
    CHECK(bandAtHz(edges, 2, 150.0f) == 1);
    CHECK(bandAtHz(edges, 2, 15000.0f) == -1);
    const std::vector<std::vector<float>> grid{{0.0f, 1.0f}};
    const auto img = buildGridImageOnAxis(grid, edges, [](float v) { return ui::viz::spectrogram(v); }, 100);
    REQUIRE(img.isValid());
    CHECK(img.getPixelAt(0, 2).getAlpha() == 0);                               // ~19 kHz: not measured
    CHECK(img.getPixelAt(0, 50) == ui::viz::spectrogram(1.0f));                 // ~630 Hz: band 1
    CHECK(img.getPixelAt(0, 95) == ui::viz::spectrogram(0.0f));                 // ~28 Hz: band 0
    const auto text = gridReadoutOnAxis(grid, edges, 60.0f, {0.0f, 0.0f, 10.0f, 100.0f}, {5.0f, 2.0f},
                                        [](float v) { return juce::String(v); });
    CHECK(text.contains("not measured"));
}

TEST_CASE("VIS-2: every spectrogram image uses the one map", "[viz][vis2]")
{
    const std::vector<std::vector<float>> frames{{0.0f, 0.5f, 1.0f}};
    const auto img = buildSpectrogramImage(frames, 1, 3);
    REQUIRE(img.isValid());
    CHECK(img.getPixelAt(0, 0) == ui::viz::spectrogram(1.0f)); // top = highest band
    CHECK(img.getPixelAt(0, 2) == ui::viz::spectrogram(0.0f));
    CHECK(juce::Colour(ui::viz::spectrogramLut()[255].getInARGBMaskOrder()) == ui::viz::spectrogram(1.0f));
}

TEST_CASE("VIS-2: the diverging map is blue - neutral - orange", "[viz][vis2]")
{
    const auto warm = juce::Colour(ui::viz::kDivergingWarm), cool = juce::Colour(ui::viz::kDivergingCool);
    CHECK(warm.getRed() > warm.getGreen());
    CHECK(warm.getGreen() > warm.getBlue()); // orange, not red or magenta
    CHECK(cool.getBlue() > cool.getRed());
    CHECK(ui::viz::diverging(1.0f) == warm);
    CHECK(ui::viz::diverging(-1.0f) == cool);
}

TEST_CASE("VIS-6: a stopped meter greys its data colours", "[viz][vis6]")
{
    ui::CorrelationMeter m;
    m.setBounds(0, 0, 200, 44);
    CHECK_FALSE(m.isStopped());
    m.setStopped(true);
    CHECK(m.isStopped());
    const auto teal = juce::Colour(0xff2ad4c5); // theme-exempt(test colour)
    CHECK(ui::viz::stoppedTint(teal).getSaturation() < teal.getSaturation());
    // Every height paints (the caption, then the scale, drop out instead of overlapping).
    for (const int h : {8, 20, 30, 44, 80}) {
        m.setSize(200, h);
        juce::Image img(juce::Image::ARGB, 200, h, true);
        juce::Graphics g(img);
        m.paintEntireComponent(g, false);
    }
}

TEST_CASE("VIS-7: reduced motion is persisted and makes ValueAnimator jump", "[viz][vis7]")
{
    test::ScopedTempDir dir("khensu_motion_");
    ui::motion::setConfigDirectory(dir.file);
    ui::motion::setReduced(true);
    CHECK(OnlineSettings::load(dir.file).reducedMotion == "on");
    ui::motion::setConfigDirectory(dir.file); // re-read from the file
    CHECK(ui::motion::reduced());
    CHECK_FALSE(ui::motion::followsSystem());

    ValueAnimator a{[] {}};
    a.setTarget(10.0f);
    a.setTarget(90.0f);
    CHECK_FALSE(a.isAnimating());
    CHECK(a.value() == 90.0f);

    ui::motion::setReduced(false);
    CHECK_FALSE(ui::motion::reduced());
    ui::motion::followSystem();
    CHECK(ui::motion::followsSystem());
    CHECK(OnlineSettings::load(dir.file).reducedMotion.isEmpty());
    CHECK(ui::motion::reduced() == ui::motion::systemPrefersReduced());
    ui::motion::setConfigDirectory(juce::File());
}

TEST_CASE("BUG-5: the output meter card never overlaps its meters", "[viz][bug5]")
{
    test::FakeAppController controller;
    MeterBridge bridge(controller, MeterBridge::Layout::Large);
    for (const auto& size : {juce::Point<int>{355, 380}, {300, 300}, {520, 420}, {800, 520}, {240, 260}}) {
        bridge.setSize(size.x, size.y);
        const juce::Rectangle<int> boxes[] = {bridge.loudnessBounds(), bridge.truePeakBounds(),
                                              bridge.correlationBounds(), bridge.scopeBounds(),
                                              bridge.widthChartBounds()};
        for (size_t i = 0; i < std::size(boxes); ++i) {
            INFO("size " << size.x << "x" << size.y << " box " << i);
            CHECK(bridge.getLocalBounds().contains(boxes[i]));
            for (size_t j = i + 1; j < std::size(boxes); ++j)
                CHECK_FALSE(boxes[i].intersects(boxes[j]));
        }
        if (size.x >= 300 && size.y >= 300) { // a usable card: every meter reads in full
            CHECK(bridge.correlationBounds().getHeight() >= 40); // caption, track and scale all fit
            CHECK(bridge.loudnessBounds().getWidth() >= 150);
        }
    }
}
