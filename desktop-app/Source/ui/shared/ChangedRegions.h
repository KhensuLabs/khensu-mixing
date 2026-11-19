// desktop-app/Source/ui/shared/ChangedRegions.h
// NEW-4 "Hear what changed": the regions /analysis/changed_regions reports
// between the version a fix was rendered from and the fix itself, and where
// that earlier version is. Header-only so it is unit-testable.
#pragma once
#include <JuceHeader.h>
#include "../../services/IRenders.h"
#include <khensu/ipc/generated/ApiSchema.h>
#include <utility>
#include <vector>

namespace khensu::changed_regions {

/// (start, end) seconds of each changed region, as the service orders them.
inline std::vector<std::pair<double, double>> fromResponse(const juce::var& v)
{
    namespace af = api::fields;
    std::vector<std::pair<double, double>> out;
    if (!(bool)v[af::ChangedRegionsResponse::comparable]) return out;
    if (auto* arr = v[af::ChangedRegionsResponse::regions].getArray())
        for (const auto& r : *arr) {
            const double a = (double)r[af::ChangedRegion::start_sec];
            const double b = (double)r[af::ChangedRegion::end_sec];
            if (b > a) out.emplace_back(a, b);
        }
    return out;
}

/// m:ss for a status line.
inline juce::String clock(double sec)
{
    const int s = juce::jmax(0, (int)std::lround(sec));
    return juce::String(s / 60) + ":" + juce::String(s % 60).paddedLeft('0', 2);
}

/// The version the current one was rendered from: its parent in the render
/// tree, or the original when it is a root (or nothing is rendered).
inline juce::String versionBefore(const IRenders& renders, const juce::String& original)
{
    const auto& tree = renders.renderTree();
    const int node = renders.currentRenderTreeNode();
    if (node < 0 || node >= (int)tree.size()) return {};
    const int parent = tree[(size_t)node].parent;
    return parent >= 0 && parent < (int)tree.size() ? tree[(size_t)parent].path : original;
}

} // namespace khensu::changed_regions
