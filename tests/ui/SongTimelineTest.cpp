// tests/ui/SongTimelineTest.cpp
// VIZ-2: the shared timeline - lanes on one time axis, findings placed where
// they happen, click to seek, a marker opens "Play this moment".
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include "ui/shared/SongTimelineView.h"
#include "ui/shared/TimelineMarkers.h"

using namespace khensu;
using Catch::Approx;

namespace {

std::shared_ptr<const TrackTimeline> fakeTimeline(double seconds)
{
    auto tl = std::make_shared<TrackTimeline>();
    tl->durationSec = seconds;
    tl->hopSec = 0.1;
    const auto hops = static_cast<size_t>(seconds / 0.1);
    tl->shortTermLufs.assign(hops, -12.0f);
    tl->momentaryLufs.assign(hops, -11.0f);
    tl->peakDbfs.assign(hops, -3.0f);
    tl->peakDbfs[50] = tl->peakDbfs[51] = 0.0f; // 5.0 - 5.2 s at full scale
    tl->truePeakEvents.push_back({20.0, 20.1, 0.4f});
    tl->truePeakEvents.push_back({40.0, 40.05, -0.3f});
    tl->bandHz = {100.0f, 1000.0f, 10000.0f};
    tl->spectrogramDb.assign(100, std::vector<float>(3, -40.0f));
    return tl;
}

IIssues::Issue issue(const std::string& code, float severity)
{
    IIssues::Issue i;
    i.recId = code + "_1";
    i.code = code;
    i.rec.severity = severity;
    i.rec.what = code + " text";
    return i;
}

} // namespace

TEST_CASE("VIZ-2c: findings are placed where they happen", "[viz][timeline]")
{
    const auto tl = fakeTimeline(60.0);
    auto hook = issue("hook_weak", 40.0f);
    hook.rec.time_range = TimeRange{30.0f, 38.0f};
    auto loudness = issue("lufs_too_quiet", 70.0f); // throughout: no marker
    std::vector<SectionData> sections(2);
    sections[0].label = "Verse 1";
    sections[0].start_sec = 0.0f;
    sections[0].end_sec = 30.0f;
    sections[0].harshness_score = 0.2f;
    sections[1].label = "Chorus 1";
    sections[1].start_sec = 30.0f;
    sections[1].end_sec = 60.0f;
    sections[1].harshness_score = 0.7f;
    const auto markers = timeline_markers::build(
        {hook, loudness, issue("true_peak_over", 50.0f), issue("clipping_detected", 80.0f), issue("harshness_detected", 45.0f)},
        tl.get(), sections);
    // clipping 5.0-5.2, true peak 20 and 40, hook 30-38, harshness in the chorus.
    REQUIRE(markers.size() == 5);
    CHECK(markers[0].startSec == Approx(5.0));
    CHECK(markers[0].endSec == Approx(5.2));
    CHECK(markers[1].startSec == Approx(20.0));
    CHECK(markers[2].key == "hook_weak_1");
    CHECK(markers[2].startSec == Approx(30.0));
    CHECK(markers[3].title.contains("Chorus 1"));
    CHECK(markers[3].highHz > markers[3].lowHz); // drawn on the spectrogram
    CHECK(markers[4].startSec == Approx(40.0));
    for (const auto& m : markers)
        CHECK(m.key != "lufs_too_quiet_1");
}

TEST_CASE("VIZ-2: one time axis, click to seek, a marker opens its moment", "[viz][timeline]")
{
    juce::ScopedJuceInitialiser_GUI init;
    SongTimelineView view;
    view.setSize(1000, 360);
    view.setTimeline(fakeTimeline(60.0));
    SongTimelineView::Marker m;
    m.key = "hook_weak_1";
    m.title = "Weak hook";
    m.severity = 40.0f;
    m.startSec = 30.0;
    m.endSec = 38.0;
    view.setMarkers({m});

    // Every lane shares the x mapping.
    for (int lane = 0; lane < SongTimelineView::kLaneCount; ++lane) {
        const auto r = view.laneArea(lane);
        CHECK(r.getX() == Approx(view.laneArea(SongTimelineView::kWaveform).getX()));
        CHECK(r.getWidth() == Approx(view.laneArea(SongTimelineView::kWaveform).getWidth()));
        CHECK(r.getHeight() > 0.0f);
    }
    CHECK(view.timeForX(view.xForTime(12.5)) == Approx(12.5).margin(1e-6));

    double seekTo = -1.0;
    view.onSeek = [&seekTo](double s) { seekTo = s; };
    auto click = [&view](juce::Point<float> at) {
        const juce::MouseEvent e(juce::Desktop::getInstance().getMainMouseSource(), at, {}, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                 &view, &view, juce::Time::getCurrentTime(), at, juce::Time::getCurrentTime(), 1, false);
        view.mouseDown(e);
        view.mouseUp(e);
    };
    click({view.xForTime(15.0), view.laneArea(SongTimelineView::kLoudness).getCentreY()});
    CHECK(seekTo == Approx(15.0).margin(0.1));

    const auto findings = view.laneArea(SongTimelineView::kFindings);
    CHECK(view.markerAt({view.xForTime(34.0), findings.getCentreY()}) == 0);
    double a = -1.0, b = -1.0;
    view.onPlayMoment = [&](double s, double e) {
        a = s;
        b = e;
    };
    click({view.xForTime(34.0), findings.getCentreY()});
    REQUIRE(view.selectedMarker().has_value());

    // The readout names the moment's values.
    const auto r = view.readoutAt(34.0);
    CHECK(r.title == "0:34.0");
    bool named = false;
    for (const auto& [text, colour] : r.lines)
        named = named || text.contains("Weak hook");
    CHECK(named);

    juce::Image img(juce::Image::ARGB, 1000, 360, true);
    juce::Graphics g(img);
    view.paintEntireComponent(g, false);
    view.setTimeline(nullptr);
    view.paintEntireComponent(g, false);
}
