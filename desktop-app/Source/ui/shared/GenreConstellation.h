// desktop-app/Source/ui/shared/GenreConstellation.h
// STU-12: the pure pieces of SonicOrbit's genre constellation, kept apart so
// they are testable without an OpenGL context.
//   - genreFamilyIndex(): every catalog genre (185 in genre_targets.json)
//     falls into one of eight families by its `parent`, one family per
//     categorical viz slot (KhensuTheme::vizCategory), "Other" last;
//   - nearestPoint(): the hover pick in screen space;
//   - trajectoryPaths(): the session's audio from the original through each
//     version the current one was rendered from (render tree), falling back
//     to the linear applied history;
//   - framing(): where a fly-to aims the camera and how far it backs off.
// THREAD_CONTEXT(Any) - pure functions.
#pragma once
#include <JuceHeader.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

namespace khensu::constellation {

inline constexpr int kFamilyCount = 8;

/// The families' display names, slot order (= categorical viz slot).
inline const std::array<const char*, kFamilyCount>& familyNames()
{
    static const std::array<const char*, kFamilyCount> names{
        NEEDS_TRANS("Pop"), NEEDS_TRANS("Hip-hop"), NEEDS_TRANS("Electronic"),      NEEDS_TRANS("Rock"),
        NEEDS_TRANS("R&B"), NEEDS_TRANS("Metal"),   NEEDS_TRANS("Roots and world"), NEEDS_TRANS("Other")};
    return names;
}

/// The family (0 .. kFamilyCount-1) of a genre with parent id `parent`.
inline int genreFamilyIndex(const std::string& parent)
{
    if (parent == "pop") return 0;
    if (parent == "hip-hop") return 1;
    if (parent == "electronic") return 2;
    if (parent == "rock" || parent == "alt-rock") return 3;
    if (parent == "r-and-b") return 4;
    if (parent == "metal") return 5;
    if (parent == "world" || parent == "folk" || parent == "jazz" || parent == "reggae" || parent == "country" ||
        parent == "blues")
        return 6;
    return kFamilyCount - 1;
}

/// Index of the point nearest `p` within `maxDist` pixels, or -1.
inline int nearestPoint(const std::vector<juce::Point<float>>& pts, juce::Point<float> p, float maxDist)
{
    int best = -1;
    float bestD2 = maxDist * maxDist;
    for (size_t i = 0; i < pts.size(); ++i) {
        const float d2 = pts[i].getDistanceSquaredFrom(p);
        if (d2 <= bestD2) {
            bestD2 = d2;
            best = static_cast<int>(i);
        }
    }
    return best;
}

/// The audio files the session went through, oldest first: `original`, then
/// each render on the render tree's path from its root to `currentNode`
/// (nodes carry `.path` and `.parent`). With no current node the linear
/// `applied` history stands in. Repeats of the previous path are dropped.
template <typename Node>
std::vector<juce::String> trajectoryPaths(const std::vector<Node>& tree, int currentNode,
                                          const std::vector<juce::String>& applied, const juce::String& original)
{
    std::vector<juce::String> chain;
    if (currentNode >= 0 && currentNode < static_cast<int>(tree.size())) {
        int guard = static_cast<int>(tree.size()); // a malformed cycle cannot loop forever
        for (int n = currentNode; n >= 0 && n < static_cast<int>(tree.size()) && guard-- > 0;
             n = tree[static_cast<size_t>(n)].parent)
            chain.push_back(tree[static_cast<size_t>(n)].path);
        std::reverse(chain.begin(), chain.end());
    } else {
        chain = applied;
    }
    std::vector<juce::String> out;
    if (original.isNotEmpty()) out.push_back(original);
    for (const auto& p : chain)
        if (p.isNotEmpty() && (out.empty() || out.back() != p)) out.push_back(p);
    return out;
}

/// A camera aim: the look-at point and the orbit distance.
struct Framing {
    float x = 0.0f, y = 0.0f, z = 0.0f;
    float zoom = 3.5f;
};

/// Frames `points` (x, y, z triples): aims at their centre and backs off far
/// enough to keep the farthest in view, within the orbit's zoom limits.
inline Framing framing(const std::vector<juce::Vector3D<float>>& points, float minZoom = 1.5f, float maxZoom = 8.0f)
{
    Framing f;
    if (points.empty()) return f;
    for (const auto& p : points) {
        f.x += p.x;
        f.y += p.y;
        f.z += p.z;
    }
    const auto n = static_cast<float>(points.size());
    f.x /= n;
    f.y /= n;
    f.z /= n;
    float radius = 0.0f;
    for (const auto& p : points)
        radius = std::max(radius,
                          std::sqrt((p.x - f.x) * (p.x - f.x) + (p.y - f.y) * (p.y - f.y) + (p.z - f.z) * (p.z - f.z)));
    // 45 degree field of view: a sphere of radius r fits at ~2.6 r; keep a margin.
    f.zoom = juce::jlimit(minZoom, maxZoom, 3.0f * radius + 1.0f);
    return f;
}

} // namespace khensu::constellation
