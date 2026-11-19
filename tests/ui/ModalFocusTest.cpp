// tests/ui/ModalFocusTest.cpp
// Every ModalCardHost overlay asks for keyboard focus from its constructor,
// before it has a parent. That grab used to be a silent no-op (a jassert in
// Debug), so ESC and Tab never reached a freshly opened modal until the user
// clicked inside it. The request is now held until the overlay is on screen.
#include <catch2/catch_test_macros.hpp>
#include "ui/shared/OnboardingWizard.h"
#include <khensu/targets/GenreDatabase.h>

TEST_CASE("A modal overlay takes keyboard focus once it is shown", "[ui][modal][a11y]")
{
    khensu::GenreDatabase db;
    REQUIRE(db.loadFromString(R"({"genres": [{"id": "pop", "name": "Pop", "lufs_target": -9.0}]})").is_ok());

    juce::Component window;
    window.setBounds(0, 0, 900, 640);
    window.addToDesktop(0);
    window.setVisible(true);

    khensu::OnboardingWizard wizard(db); // requests focus while still parentless
    CHECK_FALSE(wizard.hasKeyboardFocus(true));

    window.addAndMakeVisible(wizard);
    wizard.setBounds(window.getLocalBounds());
    CHECK(wizard.hasKeyboardFocus(true));

    window.removeChildComponent(&wizard);
    window.removeFromDesktop();
}
