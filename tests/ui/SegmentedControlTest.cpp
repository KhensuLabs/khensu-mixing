// tests/ui/SegmentedControlTest.cpp
#include <catch2/catch_test_macros.hpp>
#include "../../desktop-app/Source/ui/shared/SegmentedControl.h"

TEST_CASE("SegmentedControl selection", "[segmented]")
{
    khensu::SegmentedControl sc;
    sc.addSegment("WAV");
    sc.addSegment("AIFF");
    sc.addSegment("FLAC");

    SECTION("default selected is 0") { REQUIRE(sc.getSelected() == 0); }
    SECTION("setSelected changes selection")
    {
        sc.setSelected(2);
        REQUIRE(sc.getSelected() == 2);
    }
    SECTION("setSelected clamps to valid range")
    {
        sc.setSelected(10);
        REQUIRE(sc.getSelected() == 2); // last valid index
    }
}
