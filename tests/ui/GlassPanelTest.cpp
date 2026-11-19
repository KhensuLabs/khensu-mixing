// tests/ui/GlassPanelTest.cpp
#include <catch2/catch_test_macros.hpp>
#include "../../desktop-app/Source/ui/shared/KhensuLookAndFeel.h"

TEST_CASE("GlassPanelVariant enum exists", "[lookandfeel]")
{
    // Verify the enum compiles and has expected values
    REQUIRE(static_cast<int>(khensu::GlassPanelVariant::Default) == 0);
    REQUIRE(static_cast<int>(khensu::GlassPanelVariant::Raised) == 1);
    REQUIRE(static_cast<int>(khensu::GlassPanelVariant::Void) == 2);
    REQUIRE(static_cast<int>(khensu::GlassPanelVariant::GlowAccent) == 3);
}
