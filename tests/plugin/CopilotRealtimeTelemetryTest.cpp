#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include "../../plugin-copilot/Source/CopilotRealtimeTelemetry.h"

using khensu::AudioRingBuffer;
using khensu::CopilotRealtimeTelemetry;

TEST_CASE("AudioRingBuffer preserves channel data across a wrap", "[plugin][cq08]")
{
    AudioRingBuffer ring;
    ring.setSize(2, 8);

    juce::AudioBuffer<float> first(2, 6);
    juce::AudioBuffer<float> second(2, 6);
    for (int i = 0; i < 6; ++i) {
        first.setSample(0, i, static_cast<float>(i + 1));
        first.setSample(1, i, static_cast<float>(101 + i));
        second.setSample(0, i, static_cast<float>(201 + i));
        second.setSample(1, i, static_cast<float>(301 + i));
    }

    ring.write(first);
    juce::AudioBuffer<float> consumed(2, 4);
    REQUIRE(ring.read(consumed) == 4);
    for (int i = 0; i < 4; ++i) {
        CHECK(consumed.getSample(0, i) == Catch::Approx(static_cast<float>(i + 1)));
        CHECK(consumed.getSample(1, i) == Catch::Approx(static_cast<float>(101 + i)));
    }

    ring.write(second);
    juce::AudioBuffer<float> remaining(2, 8);
    // AbstractFifo keeps one slot open to distinguish full from empty.
    REQUIRE(ring.read(remaining) == 7);
    for (int i = 0; i < 2; ++i) {
        CHECK(remaining.getSample(0, i) == Catch::Approx(static_cast<float>(i + 5)));
        CHECK(remaining.getSample(1, i) == Catch::Approx(static_cast<float>(105 + i)));
    }
    for (int i = 0; i < 5; ++i) {
        CHECK(remaining.getSample(0, i + 2) == Catch::Approx(static_cast<float>(201 + i)));
        CHECK(remaining.getSample(1, i + 2) == Catch::Approx(static_cast<float>(301 + i)));
    }
}

TEST_CASE("Realtime telemetry mirrors mono meters and publishes audio", "[plugin][cq08]")
{
    CopilotRealtimeTelemetry telemetry;
    telemetry.prepareToPlay(48000.0, 1);

    juce::AudioBuffer<float> input(1, 480);
    input.clear();
    input.setSample(0, 0, 0.5f);
    juce::AudioBuffer<float> expected(2, 480);
    expected.clear();
    expected.setSample(0, 0, 0.5f);

    telemetry.process(input);
    const auto snapshot = telemetry.snapshot();
    CHECK(snapshot.left_peak_db == Catch::Approx(-6.0206f).margin(0.01f));
    CHECK(snapshot.right_peak_db == Catch::Approx(snapshot.left_peak_db).margin(0.001f));
    CHECK(snapshot.correlation == Catch::Approx(1.0f).margin(0.001f));
    CHECK(snapshot.width == Catch::Approx(0.0f).margin(0.001f));
    REQUIRE(telemetry.getRingBufferAvailable() == 480);
    REQUIRE(telemetry.readRingBuffer(expected) == 480);
    CHECK(expected.getSample(0, 0) == Catch::Approx(0.5f));
    // Mono writes mirror channel 0 into the right ring channel too (audit
    // L11), consistent with the mirrored right peak meter above.
    CHECK(expected.getSample(1, 0) == Catch::Approx(0.5f));
}

TEST_CASE("Mono ring writes keep the right channel live across a wrap", "[plugin][l11]")
{
    AudioRingBuffer ring;
    ring.setSize(2, 8);

    juce::AudioBuffer<float> mono(1, 6);
    for (int i = 0; i < 6; ++i) mono.setSample(0, i, static_cast<float>(i + 1));
    ring.write(mono);

    juce::AudioBuffer<float> first(2, 4);
    REQUIRE(ring.read(first) == 4);
    ring.write(mono); // wraps: 6 more into a ring with 2 already queued

    juce::AudioBuffer<float> rest(2, 8);
    REQUIRE(ring.read(rest) == 7);
    for (int i = 0; i < 7; ++i) CHECK(rest.getSample(1, i) == Catch::Approx(rest.getSample(0, i)));
    for (int i = 0; i < 4; ++i) CHECK(first.getSample(1, i) == Catch::Approx(static_cast<float>(i + 1)));
}
