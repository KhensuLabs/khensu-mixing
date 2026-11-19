// tests/ui/VocalUndoHistoryTest.cpp
// Unit coverage for the vocal strategy/macro undo/redo stack extracted from
// AppController (audit M4). Pins the push/undo/redo mechanics, the redo-tail
// invalidation, and the 50-deep cap - previously untested inside AppController.
#include <catch2/catch_test_macros.hpp>
#include "session/VocalUndoHistory.h"

using khensu::VocalUndoHistory;
using Snapshot = VocalUndoHistory::Snapshot;

namespace {
Snapshot snap(const juce::String& strategy, float comp = 0.0f) { return Snapshot{strategy, {{"Compression", comp}}}; }
} // namespace

TEST_CASE("VocalUndoHistory starts empty")
{
    VocalUndoHistory h;
    REQUIRE_FALSE(h.canUndo());
    REQUIRE_FALSE(h.canRedo());
    REQUIRE(h.undoDepth() == 0);
    REQUIRE(h.redoDepth() == 0);
}

TEST_CASE("VocalUndoHistory push records an undo point and clears redo")
{
    VocalUndoHistory h;
    h.push(snap("clean"));
    REQUIRE(h.canUndo());
    REQUIRE_FALSE(h.canRedo());
    REQUIRE(h.undoDepth() == 1);
}

TEST_CASE("VocalUndoHistory undo returns the pushed snapshot and enables redo")
{
    VocalUndoHistory h;
    h.push(snap("clean", 10.0f)); // undo point captured at "clean"
    // Current state has since moved to "bright"; undo should restore "clean".
    Snapshot restored = h.undo(snap("bright", 20.0f));
    REQUIRE(restored.strategy == "clean");
    REQUIRE(restored.macros.at("Compression") == 10.0f);
    REQUIRE_FALSE(h.canUndo());
    REQUIRE(h.canRedo());
}

TEST_CASE("VocalUndoHistory redo returns the state undo moved aside")
{
    VocalUndoHistory h;
    h.push(snap("clean"));
    h.undo(snap("bright")); // redo stack now holds "bright"
    Snapshot redone = h.redo(snap("clean"));
    REQUIRE(redone.strategy == "bright");
    REQUIRE(h.canUndo());
    REQUIRE_FALSE(h.canRedo());
}

TEST_CASE("VocalUndoHistory push invalidates the redo tail")
{
    VocalUndoHistory h;
    h.push(snap("a"));
    h.undo(snap("b")); // redo has "b"
    REQUIRE(h.canRedo());
    h.push(snap("c")); // a fresh edit clears the redo future
    REQUIRE_FALSE(h.canRedo());
}

TEST_CASE("VocalUndoHistory caps the undo stack at 50 deep")
{
    VocalUndoHistory h;
    for (int i = 0; i < 60; ++i)
        h.push(snap(juce::String(i)));
    REQUIRE(h.undoDepth() == 50);
    // The oldest entries were dropped, so the top should still be the last push.
    Snapshot top = h.undo(snap("current"));
    REQUIRE(top.strategy == "59");
}

TEST_CASE("VocalUndoHistory clear empties both stacks")
{
    VocalUndoHistory h;
    h.push(snap("a"));
    h.undo(snap("b"));
    h.clear();
    REQUIRE_FALSE(h.canUndo());
    REQUIRE_FALSE(h.canRedo());
}
