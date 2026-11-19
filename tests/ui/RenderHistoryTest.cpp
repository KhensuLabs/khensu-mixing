// tests/ui/RenderHistoryTest.cpp
// Unit coverage for the render-history state machine extracted from
// AppController (audit M4). These pin the linear undo/redo + branch-preserving
// tree semantics that were previously untested inside AppController.
#include <catch2/catch_test_macros.hpp>
#include "session/RenderHistory.h"

using khensu::RenderHistory;

TEST_CASE("RenderHistory starts on the original source")
{
    RenderHistory h;
    REQUIRE(h.currentIndex() == -1);
    REQUIRE(h.currentPath().isEmpty());
    REQUIRE_FALSE(h.canUndo());
    REQUIRE_FALSE(h.canRedo());
    REQUIRE(h.applied().empty());
    REQUIRE(h.tree().empty());
    REQUIRE(h.currentNode() == -1);
}

TEST_CASE("RenderHistory registers renders as the moving tip")
{
    RenderHistory h;
    h.registerRender("a.wav");
    h.registerRender("b.wav");
    REQUIRE(h.currentIndex() == 1);
    REQUIRE(h.currentPath() == "b.wav");
    REQUIRE(h.canUndo());
    REQUIRE_FALSE(h.canRedo());
    REQUIRE(h.applied().size() == 2);
}

TEST_CASE("RenderHistory undo/redo walk the linear stack")
{
    RenderHistory h;
    h.registerRender("a.wav");
    h.registerRender("b.wav");

    h.undo();
    REQUIRE(h.currentIndex() == 0);
    REQUIRE(h.currentPath() == "a.wav");
    REQUIRE(h.canRedo());

    h.undo();
    REQUIRE(h.currentIndex() == -1);
    REQUIRE(h.currentPath().isEmpty()); // back on the original
    REQUIRE_FALSE(h.canUndo());

    h.redo();
    REQUIRE(h.currentPath() == "a.wav");
}

TEST_CASE("RenderHistory undo/redo are clamped at the ends")
{
    RenderHistory h;
    h.registerRender("a.wav");
    h.undo();
    h.undo();
    h.undo(); // over-undo is a no-op past -1
    REQUIRE(h.currentIndex() == -1);
    h.redo();
    h.redo();
    h.redo(); // over-redo clamps at the tip
    REQUIRE(h.currentIndex() == 0);
    REQUIRE(h.currentPath() == "a.wav");
}

TEST_CASE("RenderHistory truncates the redo tail on a new render")
{
    RenderHistory h;
    h.registerRender("a.wav");
    h.registerRender("b.wav");
    h.undo();                  // now on a.wav, b.wav is a redo future
    h.registerRender("c.wav"); // applying invalidates b.wav
    REQUIRE(h.applied().size() == 2);
    REQUIRE(h.applied()[0] == "a.wav");
    REQUIRE(h.applied()[1] == "c.wav");
    REQUIRE_FALSE(h.canRedo());
    REQUIRE(h.currentPath() == "c.wav");
}

TEST_CASE("RenderHistory tree preserves abandoned branches")
{
    RenderHistory h;
    h.registerRender("a.wav"); // node 0, parent -1
    h.registerRender("b.wav"); // node 1, parent -1 (rendered from the original)
    h.undo();                  // linear tip back on a.wav
    h.registerRender("c.wav"); // node 2, parent -1

    // The linear stack truncated b.wav, but the tree still holds it.
    REQUIRE(h.tree().size() == 3);
    REQUIRE(h.tree()[0].path == "a.wav");
    REQUIRE(h.tree()[0].parent == -1);
    REQUIRE(h.tree()[1].path == "b.wav"); // abandoned branch survives
    REQUIRE(h.tree()[2].path == "c.wav");
    REQUIRE(h.tree()[2].parent == -1);
    REQUIRE(h.currentNode() == 2);
}

TEST_CASE("RenderHistory restoreTreeNode re-syncs the linear tip")
{
    RenderHistory h;
    h.registerRender("a.wav"); // node 0
    h.registerRender("b.wav"); // node 1

    const juce::String restored = h.restoreTreeNode(0);
    REQUIRE(restored == "a.wav");
    REQUIRE(h.currentNode() == 0);
    REQUIRE(h.currentPath() == "a.wav"); // linear tip re-synced to the node
    REQUIRE(h.applied().back() == "a.wav");

    // Out-of-range index is a no-op returning "".
    const int nodesBefore = (int)h.tree().size();
    REQUIRE(h.restoreTreeNode(99).isEmpty());
    REQUIRE((int)h.tree().size() == nodesBefore);
}

TEST_CASE("RenderHistory clear resets to the original")
{
    RenderHistory h;
    h.registerRender("a.wav");
    h.registerRender("b.wav");
    h.clear();
    REQUIRE(h.currentIndex() == -1);
    REQUIRE(h.applied().empty());
    REQUIRE(h.tree().empty());
    REQUIRE(h.currentNode() == -1);
}

TEST_CASE("RenderHistory: a derived render after undo is parented to the undone-to node", "[render][audit]")
{
    khensu::RenderHistory h;
    h.registerDerivedRender("a.wav"); // node 0, root
    h.registerDerivedRender("b.wav"); // node 1, parent 0
    h.undo();                         // back to a.wav (node 0)
    CHECK(h.currentNode() == 0);
    h.registerDerivedRender("c.wav"); // must branch from a.wav, not b.wav
    REQUIRE(h.tree().size() == 3);
    CHECK(h.tree()[1].parent == 0);
    CHECK(h.tree()[2].parent == 0);
    h.undo();
    h.undo(); // original source
    CHECK(h.currentNode() == -1);
    h.registerDerivedRender("d.wav");
    CHECK(h.tree()[3].parent == -1);
}

TEST_CASE("RenderHistory: renders made from the original are children of the root", "[render][audit064]")
{
    // AUDIT-064: every render entry point renders the ORIGINAL source, so an
    // EQ render followed by a fix render is two siblings of the original -
    // the fix does not contain the EQ, and the tree must not draw a chain.
    khensu::RenderHistory h;
    h.registerRender("eq.wav");
    h.registerRender("fix.wav");
    REQUIRE(h.tree().size() == 2);
    CHECK(h.tree()[0].parent == -1);
    CHECK(h.tree()[1].parent == -1);
    // Linear undo still steps back through what was auditioned.
    CHECK(h.currentPath() == "fix.wav");
    h.undo();
    CHECK(h.currentPath() == "eq.wav");
    CHECK(h.currentNode() == 0);
    CHECK(h.findNode("fix.wav") == 1);
    CHECK(h.findNode("missing.wav") == -1);
}

TEST_CASE("RenderHistory::baselineFor names the audio a render was made from (CE-PLT-2)")
{
    RenderHistory h;
    REQUIRE(RenderHistory::baselineFor(h.tree(), h.currentNode(), "orig.wav").isEmpty()); // original: no baseline
    h.registerRender("a.wav");                                                            // root render
    REQUIRE(RenderHistory::baselineFor(h.tree(), h.currentNode(), "orig.wav") == "orig.wav");
    h.registerDerivedRender("b.wav"); // rendered from a.wav
    REQUIRE(RenderHistory::baselineFor(h.tree(), h.currentNode(), "orig.wav") == "a.wav");
    REQUIRE(RenderHistory::baselineFor(h.tree(), 99, "orig.wav").isEmpty());
    REQUIRE(RenderHistory::baselineFor(h.tree(), -1, "orig.wav").isEmpty());
}

TEST_CASE("RenderHistory::keptInTree: a render is kept only while the current version is built on it (FIX-3)",
          "[renderhistory][fix3]")
{
    using K = RenderHistory::Kept;
    const auto kept = [](const RenderHistory& h, const char* path) {
        return RenderHistory::keptInTree(h.tree(), h.currentNode(), path);
    };
    RenderHistory h;
    h.registerRenderFrom("fixA.wav", {});
    CHECK(kept(h, "fixA.wav") == K::Kept);
    h.registerRenderFrom("fixB.wav", "fixA.wav"); // stacked on A: both are in the audio
    CHECK(kept(h, "fixA.wav") == K::Kept);
    CHECK(kept(h, "fixB.wav") == K::Kept);

    // "Start from original", then another fix: the current version has
    // neither A nor B in it, though both still sit before it in the list.
    h.registerRenderFrom("fixC.wav", {});
    CHECK(kept(h, "fixA.wav") == K::Undone);
    CHECK(kept(h, "fixB.wav") == K::Undone);
    CHECK(kept(h, "fixC.wav") == K::Kept);

    h.undo(); // back on B, which is built on A
    CHECK(kept(h, "fixA.wav") == K::Kept);
    CHECK(kept(h, "fixB.wav") == K::Kept);
    CHECK(kept(h, "fixC.wav") == K::Undone);

    h.undo();
    h.undo(); // the original
    CHECK(h.currentPath().isEmpty());
    CHECK(kept(h, "fixA.wav") == K::Undone);
    h.redo();
    CHECK(kept(h, "fixA.wav") == K::Kept);
    CHECK(kept(h, "fixB.wav") == K::Undone);

    // A render made after an undo does not erase the undone one: it is
    // still in the tree and can be restored.
    h.registerRenderFrom("fixD.wav", "fixA.wav");
    CHECK(kept(h, "fixB.wav") == K::Undone);
    h.restoreTreeNode(h.findNode("fixB.wav"));
    CHECK(kept(h, "fixB.wav") == K::Kept);
    CHECK(kept(h, "fixA.wav") == K::Kept);
    CHECK(kept(h, "fixD.wav") == K::Undone);

    CHECK(kept(h, "never.wav") == K::Gone);
    h.clear(); // a session reset
    CHECK(kept(h, "fixA.wav") == K::Gone);
    CHECK(RenderHistory::keptInTree({}, -1, "a.wav") == K::Gone);
    // A corrupt parent link must not loop.
    const std::vector<RenderHistory::TreeNode> loop{{"x.wav", 1}, {"y.wav", 0}};
    CHECK(RenderHistory::keptInTree(loop, 0, "z.wav") == K::Gone);
    CHECK(RenderHistory::keptInTree(loop, 0, "y.wav") == K::Kept);
}

TEST_CASE("RenderHistory::registerRenderFrom parents a render at the version it processed (WF-1)", "[renderhistory]")
{
    RenderHistory h;
    h.registerRenderFrom("fix1.wav", {});           // from the original: a root
    h.registerRenderFrom("fix2.wav", "fix1.wav");   // stacked on fix 1
    REQUIRE(h.tree().size() == 2);
    CHECK(h.tree()[0].parent == -1);
    CHECK(h.tree()[1].parent == 0);
    CHECK(h.currentPath() == "fix2.wav");

    // Finishing after the user stepped back still lands under its input.
    h.undo();
    h.undo();
    REQUIRE(h.currentPath().isEmpty());
    h.registerRenderFrom("fix3.wav", "fix2.wav");
    CHECK(h.tree()[2].parent == 1);
    CHECK(h.currentPath() == "fix3.wav");

    // An input the tree doesn't hold is treated as a root.
    h.registerRenderFrom("fix4.wav", "elsewhere.wav");
    CHECK(h.tree()[3].parent == -1);
}
