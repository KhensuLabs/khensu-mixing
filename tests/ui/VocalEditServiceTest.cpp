// tests/ui/VocalEditServiceTest.cpp
// Pins VocalEditService (ENG-1 stage C3, extracted from AppController): the
// strategy / macro setters and their undo/redo round trip, the
// isUndoingRedoing_ guard (an undo never pushes a new undo point), autosave
// restores that do not push undo or fire onChanged, and the session reset.
#include <catch2/catch_test_macros.hpp>
#include "session/VocalEditService.h"

using khensu::VocalEditService;

namespace {
struct Fixture {
    VocalEditService edit;
    int changes = 0;
    Fixture()
    {
        edit.onChanged = [this] { ++changes; };
    }
};
} // namespace

TEST_CASE("VocalEditService starts on the defaults", "[split][vocaledit]")
{
    Fixture f;
    CHECK(f.edit.getSelectedVocalStrategy() == "clean");
    CHECK_FALSE(f.edit.hasExplicitVocalStrategy());
    CHECK(f.edit.getVocalMacroValue("Compression") == 75.0f);
    CHECK(f.edit.getVocalMacroValue("Reverb") == 30.0f);
    CHECK(f.edit.getVocalMacroValue("NoSuchMacro") == 0.0f);
    CHECK(f.edit.macroValues() == VocalEditService::defaultVocalMacros());
    CHECK_FALSE(f.edit.canUndo());
    CHECK_FALSE(f.edit.canRedo());
}

TEST_CASE("VocalEditService undoes and redoes strategy and macro edits", "[split][vocaledit]")
{
    Fixture f;
    f.edit.setSelectedVocalStrategy("aggressive");
    CHECK(f.edit.hasExplicitVocalStrategy());
    CHECK(f.edit.getSelectedVocalStrategy() == "aggressive");
    CHECK(f.changes == 1);
    REQUIRE(f.edit.canUndo());

    // Setting the same strategy again is a no-op (but still explicit).
    f.edit.setSelectedVocalStrategy("aggressive");
    CHECK(f.changes == 1);

    f.edit.pushUndoState(); // macro edits push explicitly (the panel does)
    f.edit.setVocalMacroValue("Reverb", 55.0f);
    CHECK(f.edit.getVocalMacroValue("Reverb") == 55.0f);
    CHECK(f.changes == 2);
    f.edit.setVocalMacroValue("Reverb", 55.0f); // unchanged: no notification
    CHECK(f.changes == 2);

    f.edit.undo(); // back to before the macro edit
    CHECK(f.edit.getVocalMacroValue("Reverb") == 30.0f);
    CHECK(f.edit.getSelectedVocalStrategy() == "aggressive");
    CHECK(f.edit.canRedo());
    f.edit.undo(); // back to before the strategy change
    CHECK(f.edit.getSelectedVocalStrategy() == "clean");
    // The undo's own setSelectedVocalStrategy did not push (isUndoingRedoing_).
    CHECK_FALSE(f.edit.canUndo());

    f.edit.redo();
    CHECK(f.edit.getSelectedVocalStrategy() == "aggressive");
    CHECK(f.edit.getVocalMacroValue("Reverb") == 30.0f);
    f.edit.redo();
    CHECK(f.edit.getVocalMacroValue("Reverb") == 55.0f);
    CHECK_FALSE(f.edit.canRedo());
    CHECK(f.edit.canUndo());

    // A fresh edit clears the redo tail.
    f.edit.undo();
    REQUIRE(f.edit.canRedo());
    f.edit.setSelectedVocalStrategy("warm");
    CHECK_FALSE(f.edit.canRedo());
}

TEST_CASE("VocalEditService restores autosaved values without undo or onChanged", "[split][vocaledit]")
{
    Fixture f;
    f.edit.restoreStrategy("airy");
    CHECK(f.edit.getSelectedVocalStrategy() == "airy");
    CHECK(f.edit.hasExplicitVocalStrategy()); // restored counts as explicit

    juce::DynamicObject saved;
    saved.setProperty("Presence", 12.5);
    saved.setProperty("NotAMacro", 99.0); // unknown keys are ignored
    f.edit.restoreMacros(saved);
    CHECK(f.edit.getVocalMacroValue("Presence") == 12.5f);
    CHECK(f.edit.getVocalMacroValue("Width") == 40.0f);
    CHECK(f.edit.macroValues().count("NotAMacro") == 0);

    CHECK_FALSE(f.edit.canUndo());
    CHECK(f.changes == 0);
}

TEST_CASE("VocalEditService resets to the defaults for a new session", "[split][vocaledit]")
{
    Fixture f;
    f.edit.setSelectedVocalStrategy("warm");
    f.edit.setVocalMacroValue("Delay", 80.0f);
    REQUIRE(f.edit.canUndo());

    f.edit.resetForSession();
    CHECK(f.edit.getSelectedVocalStrategy() == "clean");
    CHECK_FALSE(f.edit.hasExplicitVocalStrategy());
    CHECK(f.edit.macroValues() == VocalEditService::defaultVocalMacros());
    CHECK_FALSE(f.edit.canUndo());
    CHECK_FALSE(f.edit.canRedo());
}

TEST_CASE("VocalEditService keeps the session's Takes bin (WF-5)", "[vocaledit][takes]")
{
    Fixture f;
    juce::TemporaryFile a(".wav"), b(".wav");
    REQUIRE(a.getFile().create().wasOk());
    REQUIRE(b.getFile().create().wasOk());
    const auto pa = a.getFile().getFullPathName(), pb = b.getFile().getFullPathName();
    int notified = 0;
    int key = 0;
    f.edit.addTakesListener(&key, [&] { ++notified; });

    // Existing files join once, in order; a missing file is skipped.
    f.edit.addSessionTakes({pa, pb, pa, "Z:/no/such/take.wav"});
    CHECK(f.edit.sessionTakes() == juce::StringArray(pa, pb));
    CHECK(notified == 1);
    CHECK(f.changes == 1); // autosaved

    f.edit.addSessionTakes({pa}); // nothing new: no notification, no autosave
    CHECK(notified == 1);
    CHECK(f.changes == 1);

    f.edit.removeSessionTake(pa);
    CHECK(f.edit.sessionTakes() == juce::StringArray(pb));
    CHECK(notified == 2);

    // A restore notifies the panels but is not a change to save again.
    f.edit.restoreTakes({pa, pb});
    CHECK(f.edit.sessionTakes().size() == 2);
    CHECK(notified == 3);
    CHECK(f.changes == 2);

    f.edit.resetForSession();
    CHECK(f.edit.sessionTakes().isEmpty());
    CHECK(notified == 4);

    f.edit.removeTakesListener(&key);
    f.edit.addSessionTakes({pa});
    CHECK(notified == 4);
}
