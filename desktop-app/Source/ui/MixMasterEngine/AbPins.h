// desktop-app/Source/ui/MixMasterEngine/AbPins.h
// STU-8: which render-tree versions the loudness-matched A/B compares. A is
// the original unless a version is pinned as A (IPlayback::abReferencePath);
// B is whatever the A/B plays as processed (IPlayback::abProcessedPath) -
// the current version by default, or a version pinned as B. Pinning only
// changes what plays: the accepted version and its fix check do not move.
// Pure, so the render tree's pin state is unit-tested (tests/ui/AbPinsTest.cpp).
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include "../../session/RenderHistory.h"
#include <vector>

namespace khensu::ab_pins {

struct State {
    int aNode = -1;           ///< tree index playing as A; -1 = the original (or a file not in the tree)
    int bNode = -1;           ///< tree index playing as B; -1 = no B, or a file not in the tree
    bool aIsOriginal = true;  ///< nothing pinned as A
    bool hasB = false;        ///< there is a B side at all
    /// Original against the current version (or no B while the original is
    /// current): what Reset A/B goes back to. Reset is offered otherwise.
    bool isDefault = true;
};

/// The tree index whose file is `path`, or -1.
inline int nodeFor(const std::vector<RenderHistory::TreeNode>& tree, const juce::String& path)
{
    if (path.isEmpty()) return -1;
    for (int i = 0; i < static_cast<int>(tree.size()); ++i)
        if (tree[static_cast<size_t>(i)].path == path) return i;
    return -1;
}

/// `aPath`: the pinned A ("" = the original). `bPath`: the A/B's processed
/// side ("" = none). `currentPath`: the accepted version ("" = the original).
inline State resolve(const std::vector<RenderHistory::TreeNode>& tree, const juce::String& aPath,
                     const juce::String& bPath, const juce::String& currentPath)
{
    State s;
    s.aIsOriginal = aPath.isEmpty();
    s.aNode = nodeFor(tree, aPath);
    s.hasB = bPath.isNotEmpty();
    s.bNode = nodeFor(tree, bPath);
    s.isDefault = s.aIsOriginal && bPath == currentPath;
    return s;
}

/// What Reset A/B sets: A back to the original (""), B to the current version.
struct Reset {
    juce::String aPath;
    juce::String bPath;
};
inline Reset resetTo(const juce::String& currentPath) { return {{}, currentPath}; }

/// The row's badge: "A", "B", "A=B" or "" for a node playing as neither.
inline juce::String badgeFor(const State& s, int node)
{
    const bool a = node >= 0 && node == s.aNode, b = node >= 0 && node == s.bNode;
    return a && b ? juce::String("A=B") : a ? juce::String("A") : b ? juce::String("B") : juce::String();
}

} // namespace khensu::ab_pins
