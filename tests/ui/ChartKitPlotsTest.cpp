// tests/ui/ChartKitPlotsTest.cpp
// VIS-1 / VIS-3 / VIS-5 / VIS-8: the chart toolkit's standard axes, the line /
// bar / stacked-area / radar charts (values, hit tests, readouts, the
// screen-reader summaries) and the tonal-balance hero's summary.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <cmath>
#include <khensu/ui/ChartKitPlots.h>
#include <khensu/ui/ChartScales.h>
#include "ui/shared/HeroSpectrumView.h"
#include "ui/shared/RadarChartComponent.h"

using Catch::Approx;
namespace chart = khensu::ui::chart;
namespace scales = khensu::ui::chart::scales;

TEST_CASE("VIS-3: one frequency axis and the standard level ranges", "[vis][chart]")
{
    const auto f = scales::frequency();
    CHECK(f.lo == 20.0);
    CHECK(f.hi == 20000.0);
    CHECK(f.scale == chart::Scale::Log);
    const auto ticks = f.ticks(12);
    CHECK(std::find(ticks.begin(), ticks.end(), 1000.0) != ticks.end());
    CHECK(f.label(20000.0, true) == "20 kHz");

    CHECK(scales::lufs().lo == -36.0);
    CHECK(scales::lufs().hi == 0.0);
    CHECK(scales::lufs().unit == "LUFS");
    CHECK(scales::spectrumDb().lo == -60.0);
    CHECK(scales::relativeDb().lo == -18.0);
    CHECK(scales::relativeDb().hi == 18.0);
    CHECK(scales::gainReduction().lo == -24.0);

    // autoFit narrows to the data inside the standard range ...
    const auto fit = scales::autoFit(scales::lufs(), -20.0, -8.0);
    CHECK(fit.lo == -24.0);
    CHECK(fit.hi == 0.0);
    // ... keeps a minimum span, and widens only for data past the range.
    const auto narrow = scales::autoFit(scales::lufs(), -14.0, -13.0, 6.0, 18.0);
    CHECK(narrow.hi - narrow.lo >= 18.0);
    const auto wide = scales::autoFit(scales::lufs(), -45.0, -10.0);
    CHECK(wide.lo <= -45.0);
}

TEST_CASE("VIS-1: category axes and inverted axes", "[vis][chart]")
{
    auto a = scales::categories({"Intro", "Verse", "Chorus"}, true);
    CHECK(a.label(1.0) == "Verse");
    CHECK(a.label(5.0).isEmpty());
    CHECK(a.ticks(10).size() == 3);
    // Inverted: the first category at the top (norm 1).
    CHECK(a.norm(0.0) > a.norm(2.0));
    CHECK(a.denorm(a.norm(1.3)) == Approx(1.3));
}

TEST_CASE("VIS-1: line chart values - interpolation, gaps, steps and dots", "[vis][chart]")
{
    chart::Series s;
    s.xs = {0.0, 1.0, 2.0, 3.0};
    s.ys = {0.0, 10.0, std::nan(""), 30.0};
    CHECK(*chart::LineChart::valueAt(s, 0.5) == Approx(5.0));
    CHECK_FALSE(chart::LineChart::valueAt(s, 1.5).has_value()); // a gap
    CHECK_FALSE(chart::LineChart::valueAt(s, -1.0).has_value());
    s.kind = chart::Series::Kind::Steps;
    CHECK(*chart::LineChart::valueAt(s, 0.9) == Approx(0.0));
    s.kind = chart::Series::Kind::Dots;
    CHECK(*chart::LineChart::valueAt(s, 2.8) == Approx(30.0));
}

TEST_CASE("VIS-1 / VIS-8: line chart readout and summary carry the units", "[vis][chart]")
{
    juce::ScopedJuceInitialiser_GUI init;
    struct Probe final : chart::LineChart {
        chart::Readout at(double x) { return readoutAt(x, 0.0); }
        juce::String auto_() const { return autoSummary(); }
        void paintData(juce::Graphics& g, juce::Rectangle<float> p) override { LineChart::paintData(g, p); }
    } c;
    c.setTitle("Loudness");
    c.x = scales::time(10.0);
    c.y = scales::lufs();
    c.setSeries({{"Short-term", juce::Colours::white, {0.0, 5.0, 10.0}, {-20.0, -10.0, -14.0}}});
    c.setSpans({{2.0, 4.0, juce::Colours::red.withAlpha(0.2f), "Chorus"}});
    const auto r = c.at(2.5);
    CHECK(r.title == "0:02.5");
    bool sawChorus = false, sawValue = false;
    for (const auto& [text, colour] : r.lines) {
        sawChorus = sawChorus || text == "Chorus";
        sawValue = sawValue || text == "Short-term -15 LUFS";
    }
    CHECK(sawChorus);
    CHECK(sawValue);
    CHECK(c.summary().contains("Loudness"));
    CHECK(c.summary().contains("LUFS"));
    c.setSummary("custom");
    CHECK(c.summary() == "custom");

    c.setSize(400, 200);
    juce::Image img(juce::Image::ARGB, 400, 200, true);
    juce::Graphics g(img);
    c.setPlayhead(5.0);
    c.setHover(std::make_pair(5.0, -12.0));
    c.paintEntireComponent(g, false);
    CHECK(c.playhead() == 5.0);

    // VIS-8: the chart is an image whose description is the summary.
    // (getAccessibilityHandler() needs a window; the handler itself is what a
    // screen reader is given.)
    auto h = c.createAccessibilityHandler();
    REQUIRE(h != nullptr);
    CHECK(h->getRole() == juce::AccessibilityRole::image);
    CHECK(h->getDescription() == "custom");
}

TEST_CASE("VIS-1: bar chart hit test, readout and summary", "[vis][chart]")
{
    juce::ScopedJuceInitialiser_GUI init;
    chart::BarChart c;
    c.setTitle("Stem energy");
    c.setHorizontal(true);
    c.x = scales::percent();
    c.setBaseline(0.0);
    chart::Bar a;
    a.label = "Vocals";
    a.values = {0.4};
    a.target = 0.3;
    chart::Bar b;
    b.label = "Drums";
    b.values = {std::nan("")};
    b.note = "not detected";
    c.setBars({a, b}, {{"Share", juce::Colours::blue}});
    CHECK(c.isHorizontal());
    CHECK(c.valueAxis().format == chart::Format::Percent);
    CHECK(c.barAt(0.2, 0.0) == 0);
    CHECK(c.barAt(0.2, 1.1) == 1);
    CHECK(c.barAt(0.2, 0.5) == -1); // between the bars
    const auto s = c.summary();
    CHECK(s.contains("Vocals 40 %"));
    CHECK(s.contains("target 30 %"));
    CHECK(s.contains("Drums not detected"));

    int clicked = -1;
    c.onBarClicked = [&clicked](int i) { clicked = i; };
    c.onClickAt(0.2, 1.0);
    CHECK(clicked == 1);

    c.setSize(300, 120);
    juce::Image img(juce::Image::ARGB, 300, 120, true);
    juce::Graphics g(img);
    c.setHover(std::make_pair(0.2, 0.0));
    c.paintEntireComponent(g, false);
}

TEST_CASE("VIS-1: stacked areas normalise to shares", "[vis][chart]")
{
    juce::ScopedJuceInitialiser_GUI init;
    chart::StackedAreaChart c;
    c.setLayers({0.0, 1.0}, {{"a", juce::Colours::red, {1.0, 3.0}}, {"b", juce::Colours::blue, {1.0, 1.0}}}, true);
    CHECK(c.y.format == chart::Format::Percent);
    CHECK(c.layerAt(1.0, 0.5) == 0);  // a is 75 % at x = 1
    CHECK(c.layerAt(1.0, 0.9) == 1);
    CHECK(c.summary().contains("a averages 62.5 %"));
}

TEST_CASE("VIS-1 / VIS-8: radar spokes, hover and summary", "[vis][chart]")
{
    juce::ScopedJuceInitialiser_GUI init;
    khensu::RadarChartComponent r;
    r.setTitle("Heaviness");
    r.setAxes({{"Distortion", 0.5f, {}}, {"Weight", 0.8f, "-14.0 LUFS"}, {"Clarity", 0.2f, {}}, {"Extremity", 0.4f, {}}});
    r.setReference({0.6f, 0.6f, 0.6f, 0.6f}, "Metal");
    r.setSize(300, 300);
    // The first spoke points up from the centre.
    const auto c = r.getLocalBounds().toFloat().getCentre();
    CHECK(r.spokeAt({c.x, c.y - 60.0f}) == 0);
    CHECK(r.spokeAt({c.x + 60.0f, c.y + 6.0f}) == 1);
    const auto ro = r.readoutFor(1);
    CHECK(ro.title == "Weight");
    REQUIRE(ro.lines.size() == 2);
    CHECK(ro.lines[0].first.contains("-14.0 LUFS"));
    CHECK(ro.lines[1].first.contains("Metal 60 %"));
    CHECK(r.summary().contains("Distortion 50 %"));
    CHECK(r.summary().contains("Metal"));
    juce::Image img(juce::Image::ARGB, 300, 300, true);
    juce::Graphics g(img);
    r.paintEntireComponent(g, false);
}

TEST_CASE("VIS-5 / VIS-8: the tonal-balance hero names its tilt and problem bands", "[vis][ui1]")
{
    juce::ScopedJuceInitialiser_GUI init;
    namespace hs = khensu::hero_spectrum;
    hs::Model m;
    for (float hz = 25.0f; hz <= 16000.0f; hz *= 1.12f)
        m.spectrum.push_back({hz, hz < 100.0f ? 6.0f : -2.0f * std::log2(hz / 1000.0f)});
    m.tiltMeasured = -3.1f;
    m.tiltTarget = -4.5f;
    m.tiltTolerance = 1.5f;
    hs::Finding low;
    low.recId = "low";
    low.code = "low_end_heavy";
    low.title = "The low end is heavy";
    low.chipLabel = "Low end";
    low.severity = 70.0f;
    low.shape = hs::Shape::Band;
    low.lowHz = 30.0f;
    low.highHz = 90.0f;
    m.findings.push_back(low);

    khensu::HeroSpectrumView view;
    view.setSize(1000, 380);
    view.setModel(m, {}, false);
    const auto s = view.summary();
    CHECK(s.contains("tilt -3.1 dB/oct"));
    CHECK(s.contains("1 problem band"));
    CHECK(s.contains("Low end -")); // below the target line there
    auto h = view.createAccessibilityHandler();
    REQUIRE(h != nullptr);
    CHECK(h->getRole() == juce::AccessibilityRole::image);
    CHECK(h->getDescription() == s);
    juce::Image img(juce::Image::ARGB, 1000, 380, true);
    juce::Graphics g(img);
    view.paintEntireComponent(g, false);
}
