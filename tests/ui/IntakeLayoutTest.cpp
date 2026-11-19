// tests/ui/IntakeLayoutTest.cpp
#include <catch2/catch_test_macros.hpp>
#include "../../desktop-app/Source/ui/SessionIntake/DropZoneComponent.h"
#include "../../desktop-app/Source/ui/SessionIntake/IntakeQueueSummary.h"

TEST_CASE("DropZoneComponent state machine", "[intake]")
{
    khensu::DropZoneComponent dz;

    SECTION("default state is Empty") { REQUIRE(dz.state() == khensu::DropZoneComponent::State::Empty); }
    SECTION("setLoaded changes state")
    {
        dz.setLoaded("track.wav", "WAV", "2:41", "44.1 kHz stereo / 24-bit");
        REQUIRE(dz.state() == khensu::DropZoneComponent::State::Loaded);
    }
    SECTION("clear returns to Empty")
    {
        dz.setLoaded("track.wav", "WAV", "2:41", "44.1 kHz stereo / 24-bit");
        dz.clear();
        REQUIRE(dz.state() == khensu::DropZoneComponent::State::Empty);
    }
}

TEST_CASE("Multi-file intake terminal summary preserves mixed outcomes", "[intake][queue]")
{
    CHECK(khensu::formatIntakeQueueCompletion(3, 0, 0) == "Intake queue complete: 3/3 analyzed");
    CHECK(khensu::formatIntakeQueueCompletion(4, 1, 0) == "Intake queue complete: 3/4 analyzed (1 failed)");
    CHECK(khensu::formatIntakeQueueCompletion(5, 1, 2) ==
          "Intake queue complete: 2/5 analyzed (1 failed) (2 cancelled)");
}
