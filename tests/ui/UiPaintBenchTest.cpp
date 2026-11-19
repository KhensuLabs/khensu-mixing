// tests/ui/UiPaintBenchTest.cpp
// Message-thread cost of the shared desktop widgets that KhensuTests links
// (the full screens need a live AppController, so the whole-app pass is the
// `--ui-bench=<file>` launch flag; see docs/archive/2026-10-17-performance-baseline.md).
// Hidden tag: KhensuTests.exe "[.bench]". Paint is timed by rendering the
// whole tree into a software image (the paint code's CPU cost), resized by
// re-laying-out at alternating sizes. Budgets are ~3-4x the baseline.
#include <catch2/catch_test_macros.hpp>
#include <JuceHeader.h>
#include "../../desktop-app/Source/ui/shared/HeatSpectrumComponent.h"
#include "../../desktop-app/Source/ui/shared/MetricTileComponent.h"
#include "../../desktop-app/Source/ui/shared/TargetRingGauge.h"
#include "../../desktop-app/Source/ui/shared/ZoneMeterComponent.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>
#include <numeric>
#include <vector>

using namespace khensu;

namespace {

/// A dashboard-sized wall of the shared widgets, populated with plausible data.
struct WidgetWall final : juce::Component {
    HeatSpectrumComponent spectrum;
    std::vector<std::unique_ptr<ZoneMeterComponent>> meters;
    std::vector<std::unique_ptr<TargetRingGauge>> gauges;
    std::vector<std::unique_ptr<MetricTileComponent>> tiles;

    WidgetWall()
    {
        std::vector<float> bands(31), mid(31), side(31);
        for (size_t i = 0; i < bands.size(); ++i) {
            bands[i] = -12.0f - 0.8f * static_cast<float>(i) + 3.0f * std::sin(static_cast<float>(i));
            mid[i] = bands[i] + 1.0f;
            side[i] = bands[i] - 8.0f;
        }
        spectrum.setSampleRate(48000.0);
        spectrum.setBands(bands);
        spectrum.setMidSideBands(mid, side);
        spectrum.setTargetTilt(-4.5f, true);
        spectrum.setGenreReference(2200.0f, 0.4f);
        addAndMakeVisible(spectrum);
        for (int i = 0; i < 8; ++i) {
            auto m = std::make_unique<ZoneMeterComponent>();
            m->setRange(-24.0f, 0.0f);
            m->setTargetZone(-16.0f, -9.0f);
            m->setValue(-14.0f + static_cast<float>(i), "LUFS");
            addAndMakeVisible(*m);
            meters.push_back(std::move(m));
            auto g = std::make_unique<TargetRingGauge>();
            g->setTarget("Crest", 8.0f + static_cast<float>(i), 10.0f, 2.0f, "dB");
            addAndMakeVisible(*g);
            gauges.push_back(std::move(g));
        }
        for (int i = 0; i < 12; ++i) {
            auto t = std::make_unique<MetricTileComponent>();
            t->setMetric("Metric " + juce::String(i), juce::String(-13.0 + i, 1), "dB",
                         i % 3 == 0 ? BadgeStatus::Warn : BadgeStatus::Pass);
            addAndMakeVisible(*t);
            tiles.push_back(std::move(t));
        }
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced(8);
        spectrum.setBounds(r.removeFromTop(r.getHeight() / 3));
        auto row = r.removeFromTop(r.getHeight() / 2);
        const int w = row.getWidth() / static_cast<int>(meters.size());
        for (size_t i = 0; i < meters.size(); ++i) {
            auto cell = row.removeFromLeft(w);
            meters[i]->setBounds(cell.removeFromTop(cell.getHeight() / 2).reduced(4));
            gauges[i]->setBounds(cell.reduced(4));
        }
        const int tw = r.getWidth() / 6;
        for (size_t i = 0; i < tiles.size(); ++i)
            tiles[i]->setBounds(r.getX() + static_cast<int>(i % 6) * tw,
                                r.getY() + static_cast<int>(i / 6) * (r.getHeight() / 2), tw - 6,
                                r.getHeight() / 2 - 6);
    }
};

/// Paints `c` (and its children) into a software ARGB image: the paint code's
/// own CPU cost. createComponentSnapshot() would use the native image type
/// (Direct2D in JUCE 8), which adds a ~86 ms GPU readback per call even for
/// an empty component and drowns the signal.
void paintToSoftwareImage(juce::Component& c)
{
    juce::Image img(juce::Image::ARGB, juce::jmax(1, c.getWidth()), juce::jmax(1, c.getHeight()), true,
                    juce::SoftwareImageType());
    juce::Graphics g(img);
    c.paintEntireComponent(g, true);
}

double ms(std::chrono::steady_clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); }

} // namespace

TEST_CASE("Bench: shared widget wall paints and lays out within a frame budget", "[.bench][perf]")
{
    WidgetWall wall;
    wall.setBounds(0, 0, 1600, 1000);
    paintToSoftwareImage(wall); // warm glyph/path caches

    constexpr int kFrames = 60;
    std::vector<double> paint, layout;
    for (int i = 0; i < kFrames; ++i) {
        auto t0 = std::chrono::steady_clock::now();
        wall.setSize(i % 2 == 0 ? 1600 : 1440, i % 2 == 0 ? 1000 : 900);
        layout.push_back(ms(std::chrono::steady_clock::now() - t0));
        t0 = std::chrono::steady_clock::now();
        paintToSoftwareImage(wall);
        paint.push_back(ms(std::chrono::steady_clock::now() - t0));
    }
    std::sort(paint.begin(), paint.end());
    const double mean = std::accumulate(paint.begin(), paint.end(), 0.0) / kFrames;
    const double p95 = paint[static_cast<size_t>(kFrames * 95 / 100)];
    const double worst = paint.back();
    const double worstLayout = *std::max_element(layout.begin(), layout.end());
    std::cout << "[bench] widget wall paint mean " << mean << " ms, p95 " << p95 << " ms, worst " << worst
              << " ms; worst layout " << worstLayout << " ms\n";
    // Per-widget-type breakdown (best of 5), so a regression names its widget.
    auto best = [](juce::Component& c) {
        double b = 1e9;
        for (int i = 0; i < 5; ++i) {
            const auto t0 = std::chrono::steady_clock::now();
            paintToSoftwareImage(c);
            b = std::min(b, ms(std::chrono::steady_clock::now() - t0));
        }
        return b;
    };
    struct TextOnly final : juce::Component {
        void paint(juce::Graphics& g) override
        {
            g.setFont(juce::FontOptions(14.0f));
            g.drawText("-13.0 LUFS", getLocalBounds(), juce::Justification::centred);
        }
    };
    juce::Component empty;
    empty.setSize(wall.tiles[0]->getWidth(), wall.tiles[0]->getHeight());
    TextOnly text;
    text.setSize(empty.getWidth(), empty.getHeight());
    std::cout << "[bench]   harness: empty component " << best(empty) << " ms, default-font text " << best(text)
              << " ms\n";
    std::cout << "[bench]   spectrum " << best(wall.spectrum) << " ms, zone meter " << best(*wall.meters[0])
              << " ms, ring gauge " << best(*wall.gauges[0]) << " ms, metric tile " << best(*wall.tiles[0])
              << " ms\n";
    CHECK(mean <= 50.0);
    CHECK(worst <= 100.0);
    CHECK(worstLayout <= 5.0);
}
