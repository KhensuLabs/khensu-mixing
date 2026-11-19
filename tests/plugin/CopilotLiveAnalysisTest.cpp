// tests/plugin/CopilotLiveAnalysisTest.cpp
// CE-DAW-3: rolling live analysis stamped with the host PlayHead position.
//  - the audio-thread handoff (LiveAnalysisFifo / pushBlock / processBlock)
//    never allocates (same thread_local guard as PluginAllocationTest);
//  - every slot and every measured window carries the host time of its audio;
//  - a host jump (loop / seek) closes the window instead of splicing audio;
//  - findings land at the time of the problem and re-measuring replaces them.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include "../../plugin-copilot/Source/PluginProcessor.h"
#include "../../plugin-copilot/Source/live/CopilotLiveAnalyzer.h"
#include "../../plugin-copilot/Source/live/LiveAnalysisFifo.h"
#include "../../plugin-copilot/Source/live/WindowIssueScanner.h"
#include "../../plugin-copilot/Source/timeline/IssueTimeline.h"
#include "AllocationGuard.h"

#include <cmath>
#include <set>
#include <string>

using Catch::Approx;
using khensu::KhensuCopilotProcessor;
using khensu::live::CopilotLiveAnalyzer;
using khensu::live::LiveAnalysisFifo;
using khensu::live::WindowIssueScanner;
using khensu::test::resetViolations;
using khensu::test::ScopedAllocationGuard;
using khensu::test::violationCount;
using khensu::timeline::IssueSource;
using khensu::timeline::IssueTimeline;
using khensu::timeline::TimedIssue;

namespace {
constexpr double kSr = 48000.0;
constexpr int kBlock = 512;

/// A host PlayHead the test moves by hand.
struct FakePlayHead final : juce::AudioPlayHead {
    double timeSec = 0.0;
    bool playing = true;
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo info;
        info.setTimeInSeconds(timeSec);
        info.setTimeInSamples(static_cast<juce::int64>(timeSec * kSr));
        info.setIsPlaying(playing);
        info.setBpm(120.0);
        return info;
    }
};

void fillSine(juce::AudioBuffer<float>& buf, double& phase, float amp, double freq = 220.0)
{
    for (int i = 0; i < buf.getNumSamples(); ++i) {
        const float v = amp * static_cast<float>(std::sin(phase));
        phase += 2.0 * juce::MathConstants<double>::pi * freq / kSr;
        for (int c = 0; c < buf.getNumChannels(); ++c) buf.setSample(c, i, v);
    }
}

/// Fully clipped square wave: true-peak / clipping findings for certain.
void fillClipped(juce::AudioBuffer<float>& buf, int& counter)
{
    for (int i = 0; i < buf.getNumSamples(); ++i, ++counter) {
        const float v = ((counter / 40) % 2 == 0) ? 1.0f : -1.0f;
        for (int c = 0; c < buf.getNumChannels(); ++c) buf.setSample(c, i, v);
    }
}
} // namespace

TEST_CASE("LiveAnalysisFifo stamps each slot with the host time of its first frame", "[plugin][cedaw3]")
{
    LiveAnalysisFifo fifo(16);
    juce::AudioBuffer<float> buf(2, 1300); // 1300 = 512 + 512 + 276
    for (int i = 0; i < 1300; ++i) {
        buf.setSample(0, i, static_cast<float>(i));
        buf.setSample(1, i, -static_cast<float>(i));
    }
    REQUIRE(fifo.push(buf.getArrayOfReadPointers(), 2, 1300, 10.0, kSr));
    REQUIRE(fifo.numReady() == 3);

    LiveAnalysisFifo::Slot slot;
    REQUIRE(fifo.pop(slot));
    CHECK(slot.timeSec == Approx(10.0));
    CHECK(slot.numSamples == 512);
    CHECK(slot.data[0][0] == 0.0f);
    REQUIRE(fifo.pop(slot));
    CHECK(slot.timeSec == Approx(10.0 + 512.0 / kSr));
    CHECK(slot.data[0][0] == 512.0f);
    CHECK(slot.data[1][0] == -512.0f);
    REQUIRE(fifo.pop(slot));
    CHECK(slot.timeSec == Approx(10.0 + 1024.0 / kSr));
    CHECK(slot.numSamples == 276);
    CHECK_FALSE(fifo.pop(slot));
}

TEST_CASE("LiveAnalysisFifo drops and counts when full instead of blocking", "[plugin][cedaw3]")
{
    LiveAnalysisFifo fifo(4); // AbstractFifo keeps one slot free: 3 usable
    juce::AudioBuffer<float> buf(2, kBlock);
    buf.clear();
    int accepted = 0;
    for (int i = 0; i < 6; ++i)
        if (fifo.push(buf.getArrayOfReadPointers(), 2, kBlock, i * 0.01, kSr)) ++accepted;
    CHECK(accepted == 3);
    CHECK(fifo.droppedSlots() == 3);
}

TEST_CASE("The live handoff does not allocate on the audio thread", "[plugin][cedaw3][cq03]")
{
    IssueTimeline timeline;
    CopilotLiveAnalyzer analyzer(timeline);
    juce::AudioBuffer<float> buf(2, kBlock);
    double phase = 0.0;
    fillSine(buf, phase, 0.3f);

    resetViolations();
    {
        ScopedAllocationGuard guard;
        for (int b = 0; b < 64; ++b)
            analyzer.pushBlock(buf.getArrayOfReadPointers(), 2, kBlock, b * kBlock / kSr, true, true, kSr);
        // Transport stop (requests a flush) and blocks without a host time.
        analyzer.pushBlock(buf.getArrayOfReadPointers(), 2, kBlock, 0.0, true, false, kSr);
        analyzer.pushBlock(buf.getArrayOfReadPointers(), 2, kBlock, 0.0, false, true, kSr);
    }
    CHECK(violationCount() == 0);
}

TEST_CASE("processBlock with live analysis and a host PlayHead does not allocate", "[plugin][cedaw3][cq03]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    KhensuCopilotProcessor proc;
    FakePlayHead playHead;
    proc.setPlayHead(&playHead);
    proc.setPlayConfigDetails(2, 2, kSr, kBlock);
    proc.prepareToPlay(kSr, kBlock);
    REQUIRE(proc.isLiveAnalysisEnabled());

    juce::AudioBuffer<float> buf(2, kBlock);
    juce::MidiBuffer midi;
    double phase = 0.0;
    auto run = [&](int blocks) {
        for (int b = 0; b < blocks; ++b) {
            fillSine(buf, phase, 0.25f);
            proc.processBlock(buf, midi);
            playHead.timeSec += kBlock / kSr;
        }
    };
    run(4); // warm-up
    resetViolations();
    {
        ScopedAllocationGuard guard;
        run(64);
    }
    CHECK(violationCount() == 0);
    // The handoff saw the host's time: the newest queued block is the last one.
    CHECK(proc.getLiveAnalyzer().lastQueuedTimeSec() == Approx(playHead.timeSec - kBlock / kSr).margin(1e-9));
    proc.setPlayHead(nullptr);
}

TEST_CASE("WindowIssueScanner stamps windows with host time and splits on a jump", "[plugin][cedaw3]")
{
    WindowIssueScanner scanner(IssueSource::Live, "live", 1.0, 0.5);
    std::vector<WindowIssueScanner::Window> windows;
    const auto sink = [&windows](WindowIssueScanner::Window&& w) { windows.push_back(std::move(w)); };

    juce::AudioBuffer<float> buf(2, kBlock);
    double phase = 0.0;
    // 2.5 s from host time 30 s: two full 1 s windows, 0.5 s pending.
    double t = 30.0;
    const int blocks = static_cast<int>(2.5 * kSr / kBlock);
    for (int b = 0; b < blocks; ++b) {
        fillSine(buf, phase, 0.2f);
        scanner.feed(buf.getArrayOfReadPointers(), 2, kBlock, t, kSr, sink);
        t += kBlock / kSr;
    }
    REQUIRE(windows.size() == 2);
    CHECK(windows[0].startSec == Approx(30.0));
    CHECK(windows[0].endSec == Approx(31.0).margin(kBlock / kSr));
    CHECK(windows[1].startSec == Approx(windows[0].endSec).margin(1e-9));

    // The host loops back to 12 s: the pending partial window (~0.5 s) is
    // closed at the jump and the next window starts at 12 s, never spliced.
    const size_t before = windows.size();
    for (int b = 0; b < static_cast<int>(1.2 * kSr / kBlock); ++b) {
        fillSine(buf, phase, 0.2f);
        scanner.feed(buf.getArrayOfReadPointers(), 2, kBlock, 12.0 + b * kBlock / kSr, kSr, sink);
    }
    REQUIRE(windows.size() >= before + 1);
    bool sawLoopWindow = false;
    for (size_t i = before; i < windows.size(); ++i) {
        // No window may straddle the jump.
        CHECK_FALSE((windows[i].startSec < 31.9 && windows[i].endSec > 32.1));
        if (windows[i].startSec == Approx(12.0)) sawLoopWindow = true;
    }
    CHECK(sawLoopWindow);
}

TEST_CASE("A clipped passage is reported at its host time and re-measuring replaces it", "[plugin][cedaw3]")
{
    IssueTimeline timeline;
    CopilotLiveAnalyzer analyzer(timeline, 1.0);
    juce::AudioBuffer<float> buf(2, kBlock);
    double phase = 0.0;
    int counter = 0;

    // Host time 60..63 s: clean, clipped, clean - one window each.
    double t = 60.0;
    for (int section = 0; section < 3; ++section) {
        for (int b = 0; b < static_cast<int>(kSr / kBlock) + 1; ++b) {
            if (section == 1)
                fillClipped(buf, counter);
            else
                fillSine(buf, phase, 0.05f);
            analyzer.pushBlock(buf.getArrayOfReadPointers(), 2, kBlock, t, true, true, kSr);
            t += kBlock / kSr;
            if (b % 16 == 0) analyzer.drainOnce(); // stay inside the FIFO capacity
        }
    }
    analyzer.pushBlock(buf.getArrayOfReadPointers(), 2, kBlock, t, true, false, kSr); // stop -> flush
    analyzer.drainOnce();
    REQUIRE(analyzer.windowsMeasured() >= 3);

    const auto issues = timeline.snapshot();
    REQUIRE_FALSE(issues.empty());
    // Windows are 1 s from 60 s; the clipped section covers ~61..62 s.
    std::set<std::string> cleanKeys, clippedKeys;
    for (const auto& issue : issues) {
        CHECK(issue.source == IssueSource::Live);
        CHECK(issue.startSec >= 60.0 - 1e-6);
        CHECK(issue.endSec <= t + 1e-6);
        const double mid = 0.5 * (issue.startSec + issue.endSec);
        if (mid >= 61.0 && mid < 62.0)
            clippedKeys.insert(issue.key);
        else if (mid < 61.0)
            cleanKeys.insert(issue.key);
    }
    // Something is reported ONLY where (when) the audio clipped.
    std::set<std::string> clippingOnly;
    for (const auto& k : clippedKeys)
        if (cleanKeys.count(k) == 0) clippingOnly.insert(k);
    REQUIRE_FALSE(clippingOnly.empty());

    // Replay that stretch clean, with windows that do not line up with the
    // first pass: the clipping-only findings are replaced.
    double t2 = 61.0 + 0.3;
    for (int b = 0; b < static_cast<int>(kSr / kBlock) + 1; ++b) {
        fillSine(buf, phase, 0.05f);
        analyzer.pushBlock(buf.getArrayOfReadPointers(), 2, kBlock, t2, true, true, kSr);
        t2 += kBlock / kSr;
        if (b % 16 == 0) analyzer.drainOnce();
    }
    analyzer.pushBlock(buf.getArrayOfReadPointers(), 2, kBlock, t2, true, false, kSr);
    analyzer.drainOnce();
    for (const auto& issue : timeline.snapshot()) {
        const double mid = 0.5 * (issue.startSec + issue.endSec);
        if (mid >= 61.0 && mid < 62.4) CHECK(clippingOnly.count(issue.key) == 0);
    }
}

TEST_CASE("IssueTimeline merges repeated windows and maps ARA sources onto regions", "[plugin][cedaw3][cedaw1]")
{
    IssueTimeline timeline;
    TimedIssue a;
    a.key = "true_peak";
    a.startSec = 10.0;
    a.endSec = 14.0;
    a.severity = 40.0f;
    TimedIssue b = a;
    b.startSec = 14.0;
    b.endSec = 18.0;
    b.severity = 80.0f;
    b.title = "worse";
    TimedIssue other = a;
    other.key = "width";
    timeline.replaceWindow(IssueSource::Live, 10.0, 18.0, {a, b, other});

    const auto merged = timeline.mergedSnapshot();
    REQUIRE(merged.size() == 2);
    const auto& tp = merged[0].key == "true_peak" ? merged[0] : merged[1];
    CHECK(tp.startSec == Approx(10.0));
    CHECK(tp.endSec == Approx(18.0));
    CHECK(tp.severity == Approx(80.0f));
    CHECK(tp.title == "worse");

    // Re-measuring 10..14 replaces only issues starting there.
    timeline.replaceWindow(IssueSource::Live, 10.0, 14.0, {});
    CHECK(timeline.snapshot().size() == 1);

    // ARA: source-time issue at 5..7 s, region plays source 4..10 s at 100 s.
    TimedIssue src;
    src.key = "clip";
    src.startSec = 5.0;
    src.endSec = 7.0;
    TimedIssue outside = src;
    outside.startSec = 20.0;
    outside.endSec = 21.0;
    TimedIssue straddle = src;
    straddle.startSec = 9.0;
    straddle.endSec = 12.0;
    const auto mapped = khensu::timeline::mapSourceIssuesToRegion({src, outside, straddle}, 4.0, 6.0, 100.0, "Vox");
    REQUIRE(mapped.size() == 2);
    CHECK(mapped[0].startSec == Approx(101.0));
    CHECK(mapped[0].endSec == Approx(103.0));
    CHECK(mapped[0].regionName == "Vox");
    CHECK(mapped[1].startSec == Approx(105.0));
    CHECK(mapped[1].endSec == Approx(106.0)); // clipped at the region end
}
