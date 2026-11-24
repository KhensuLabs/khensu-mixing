// tests/ui/HiResSpectrogramTest.cpp - VIZ-S1: the detailed spectrogram's
// mapping (time / frequency / note), its measuring options and that a
// measured tone lands where the axis says it is.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "../../desktop-app/Source/ui/shared/HiResSpectrogramView.h"
#include <khensu/analysis/TrackTimeline.h>
#include <cmath>
#include <vector>

using namespace khensu;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {
std::shared_ptr<const TrackTimeline> toneTimeline(float hz, double seconds = 4.0)
{
    const double sr = 44100.0;
    const int n = static_cast<int>(sr * seconds);
    std::vector<float> x(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
        x[static_cast<size_t>(i)] = 0.5f * std::sin(2.0f * 3.14159265f * hz * static_cast<float>(i / sr));
    return std::make_shared<const TrackTimeline>(
        measureTrackTimeline({x.data()}, n, sr, HiResSpectrogramView::measuringOptions()));
}
} // namespace

TEST_CASE("HiResSpectrogramView: note names", "[viz][spectrogram]")
{
    CHECK(HiResSpectrogramView::noteName(440.0f) == "A4");
    CHECK(HiResSpectrogramView::noteName(261.63f) == "C4");
    CHECK(HiResSpectrogramView::noteName(55.0f) == "A1");
    CHECK(HiResSpectrogramView::noteName(0.0f).isEmpty());
}

TEST_CASE("HiResSpectrogramView: measuring options are the fine grid", "[viz][spectrogram]")
{
    const auto o = HiResSpectrogramView::measuringOptions();
    CHECK(o.bands >= 200);
    CHECK(o.fftOrder == 12);
    CHECK(o.fineLowEnd);
}

TEST_CASE("HiResSpectrogramView: time and frequency map both ways", "[viz][spectrogram]")
{
    HiResSpectrogramView v;
    v.setBounds(0, 0, 900, 320);
    v.setTimeline(toneTimeline(1000.0f));
    const auto plot = v.plotArea();
    REQUIRE_FALSE(plot.isEmpty());
    CHECK_THAT(v.timeAtX(plot.getX()), WithinAbs(0.0, 1e-6));
    CHECK_THAT(v.timeAtX(v.xAtTime(2.5)), WithinAbs(2.5, 1e-3));
    CHECK_THAT(v.hzAtY(v.yAtHz(440.0f)), WithinRel(440.0f, 0.01f));
    CHECK(v.yAtHz(100.0f) > v.yAtHz(1000.0f)); // low frequencies at the bottom
}

TEST_CASE("HiResSpectrogramView: a tone is brightest in its own band, smooth low end", "[viz][spectrogram]")
{
    for (float hz : {60.0f, 1000.0f}) {
        const auto tl = toneTimeline(hz);
        REQUIRE_FALSE(tl->spectrogramDb.empty());
        const auto& row = tl->spectrogramDb[tl->spectrogramDb.size() / 2];
        size_t best = 0;
        for (size_t b = 1; b < row.size(); ++b)
            if (row[b] > row[best]) best = b;
        CHECK_THAT(tl->bandHz[best], WithinRel(hz, 0.06f));
    }
    // The low end is interpolated, not a staircase of repeated bins: no two
    // neighbouring bands below 100 Hz of a 60 Hz tone carry identical values.
    const auto tl = toneTimeline(60.0f);
    const auto& row = tl->spectrogramDb[tl->spectrogramDb.size() / 2];
    int repeats = 0;
    for (size_t b = 1; b < row.size() && tl->bandHz[b] < 100.0f; ++b)
        if (row[b] == row[b - 1]) ++repeats;
    CHECK(repeats == 0);
}

TEST_CASE("HiResSpectrogramView: paints with and without a timeline", "[viz][spectrogram]")
{
    HiResSpectrogramView v;
    v.setBounds(0, 0, 700, 260);
    juce::Image img(juce::Image::ARGB, 700, 260, true);
    {
        juce::Graphics g(img);
        v.paintEntireComponent(g, false); // the status line, no crash
    }
    v.setTimeline(toneTimeline(440.0f));
    {
        juce::Graphics g(img);
        v.paintEntireComponent(g, false);
    }
    // The plot's centre column at the tone's height is bright (top of the map).
    const auto plot = v.plotArea();
    const auto c = img.getPixelAt(juce::roundToInt(plot.getCentreX()), juce::roundToInt(v.yAtHz(440.0f)));
    CHECK(c.getBrightness() > 0.6f);
}
