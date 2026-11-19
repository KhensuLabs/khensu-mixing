// tests/ui/PanelDockTest.cpp
// Dockable/resizable panels: dock state persistence, height clamping, and
// the pop-out -> dock-back round trip re-parenting a live component.
#include <catch2/catch_test_macros.hpp>
#include "ui/shared/PanelDockManager.h"
#include "ui/shared/PanelDockState.h"

using khensu::PanelDockManager;
using khensu::PanelDockState;

TEST_CASE("Dock state persists popped-out windows and clamped heights", "[dock]")
{
    PanelDockState s;
    s.setPoppedOut("spectrogram", {100, 120, 640, 480});
    s.setHeight("chords", 20);    // clamped up
    s.setHeight("phase", 99999);  // clamped down
    CHECK(s.isPoppedOut("spectrogram"));
    CHECK(s.heightFor("chords", 260) == PanelDockState::kMinHeight);
    CHECK(s.heightFor("phase", 260) == PanelDockState::kMaxHeight);
    CHECK(s.heightFor("other", 260) == 260);

    PanelDockState r;
    r.fromVar(juce::JSON::parse(juce::JSON::toString(s.toVar())));
    REQUIRE(r.windowBounds("spectrogram").has_value());
    CHECK(*r.windowBounds("spectrogram") == juce::Rectangle<int>(100, 120, 640, 480));
    CHECK(r.hasCustomHeight("chords"));
    r.resetHeight("chords");
    CHECK_FALSE(r.hasCustomHeight("chords"));
    r.setDocked("spectrogram");
    CHECK(r.poppedOutIds().empty());
}

TEST_CASE("Pop-out moves a live panel into a window and docks it back", "[dock]")
{
    juce::ScopedJuceInitialiser_GUI init;
    juce::Component screen;
    juce::Component panel;
    screen.setBounds(0, 0, 800, 600);
    screen.addAndMakeVisible(panel);
    panel.setBounds(10, 10, 300, 200);

    int layouts = 0, saves = 0;
    {
        PanelDockManager dock([&] { ++layouts; }, [&] { ++saves; });
        dock.popOut("eq", panel, "EQ Curve");
        CHECK(dock.isPoppedOut("eq"));
        CHECK(panel.getParentComponent() != &screen);
        CHECK(dock.state().isPoppedOut("eq"));
        CHECK(layouts == 1);

        dock.dock("eq");
        CHECK_FALSE(dock.isPoppedOut("eq"));
        CHECK(panel.getParentComponent() == &screen);
        CHECK(layouts == 2);
        CHECK(saves >= 2);

        dock.popOut("eq", panel, "EQ Curve");
        // Destroying the manager re-parents the panel without forgetting
        // that it was floating (restored next launch).
    }
    CHECK(panel.getParentComponent() == &screen);
}
