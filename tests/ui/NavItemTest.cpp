// tests/ui/NavItemTest.cpp
#include <catch2/catch_test_macros.hpp>
#include "../../desktop-app/Source/ui/shared/NavItemComponent.h"

TEST_CASE("NavItemComponent active state", "[navitem]")
{
    khensu::NavItemComponent item;
    item.setLabel("Analysis");

    SECTION("default state is inactive") { REQUIRE_FALSE(item.isActive()); }
    SECTION("setActive(true) makes it active")
    {
        item.setActive(true);
        REQUIRE(item.isActive());
    }
}

TEST_CASE("NavItemComponent exposes an accessible button action", "[navitem][accessibility]")
{
    khensu::NavItemComponent item;
    item.setLabel("Analysis");

    CHECK(item.getTitle() == "Analysis");
    CHECK(item.getDescription() == "Open this screen.");
    CHECK(item.getHelpText() == "Press Return or Space to open this screen.");

    bool activated = false;
    item.onClick = [&activated] { activated = true; };
    auto handler = item.createAccessibilityHandler();
    REQUIRE(handler != nullptr);
    CHECK(handler->getRole() == juce::AccessibilityRole::button);
    REQUIRE(handler->getActions().contains(juce::AccessibilityActionType::press));
    CHECK(handler->getActions().invoke(juce::AccessibilityActionType::press));
    CHECK(activated);
}
