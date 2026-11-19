// tests/plugin/CaptureRecorderTest.cpp
// FEAT-7: the plugin's capture recorder - waits for playback, records into a
// buffer allocated when armed, never allocates on the audio thread.
#include <catch2/catch_test_macros.hpp>
#include "../../plugin-copilot/Source/CaptureRecorder.h"
#include "AllocationGuard.h"

using khensu::copilot::CaptureRecorder;
using State = CaptureRecorder::State;

namespace {
juce::AudioBuffer<float> block(int channels, int n, float value)
{
    juce::AudioBuffer<float> b(channels, n);
    for (int ch = 0; ch < channels; ++ch)
        juce::FloatVectorOperations::fill(b.getWritePointer(ch), value + (float)ch, n);
    return b;
}
} // namespace

TEST_CASE("a capture starts on playback and stops when full (FEAT-7)", "[plugin][capture]")
{
    CaptureRecorder rec;
    CHECK_FALSE(rec.arm(0, 2));
    REQUIRE(rec.arm(1000, 2));
    CHECK_FALSE(rec.arm(1000, 2)); // busy
    const auto b = block(2, 400, 0.25f);

    rec.process(b, false); // stopped: still waiting
    CHECK(rec.state() == State::Armed);
    khensu::test::resetViolations();
    {
        khensu::test::ScopedAllocationGuard guard; // the audio thread never allocates
        rec.process(b, true);
        rec.process(b, true);
    }
    CHECK(khensu::test::violationCount() == 0);
    CHECK(rec.state() == State::Recording);
    CHECK(rec.progress() == 0.8);
    rec.process(b, true); // only 200 more fit
    CHECK(rec.state() == State::Done);
    CHECK(rec.recordedSamples() == 1000);
    CHECK(rec.buffer().getSample(0, 999) == 0.25f);
    CHECK(rec.buffer().getSample(1, 0) == 1.25f);
    rec.process(b, true); // a done capture is left alone
    CHECK(rec.recordedSamples() == 1000);

    rec.reset();
    CHECK(rec.state() == State::Idle);
}

TEST_CASE("a capture ends early when playback stops, and can be cancelled (FEAT-7)", "[plugin][capture]")
{
    CaptureRecorder rec;
    REQUIRE(rec.arm(48000, 2));
    rec.process(block(1, 256, 0.5f), true); // a mono input fills both channels
    CHECK(rec.buffer().getSample(1, 10) == 0.5f);
    rec.process(block(1, 256, 0.5f), false);
    CHECK(rec.state() == State::Done);
    CHECK(rec.recordedSamples() == 256);

    REQUIRE(rec.arm(48000, 2)); // a done capture can be re-armed
    rec.cancel();
    CHECK(rec.state() == State::Idle);
    REQUIRE(rec.arm(48000, 2));
    rec.process(block(2, 64, 0.1f), true);
    rec.cancel();
    CHECK(rec.state() == State::Cancelling);
    rec.process(block(2, 64, 0.1f), true); // the audio thread lets go
    CHECK(rec.state() == State::Idle);
}
