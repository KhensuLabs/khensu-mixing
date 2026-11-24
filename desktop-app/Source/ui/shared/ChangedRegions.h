// desktop-app/Source/ui/shared/ChangedRegions.h
// NEW-4 "Hear what changed": the regions /analysis/changed_regions reports
// between the version a fix was rendered from and the fix itself, and where
// that earlier version is. Header-only so it is unit-testable.
#pragma once
#include <khensu/analysis/TrackTimeline.h>
#include <algorithm>
#include <cmath>
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

/// VIZ-3b: the changed regions measured natively from the two versions'
/// timelines - where the short-term loudness moves by more than `lu`, or the
/// spectrum (per band, mean absolute) by more than `db` - merged when closer
/// than `gapSec`, at least `minSec` long. The engine's /analysis/changed_regions
/// answers first; this is the answer when it can't.
inline std::vector<std::pair<double, double>> fromTimelines(const TrackTimeline& before, const TrackTimeline& after,
                                                            float lu = 0.5f, float db = 1.5f, double gapSec = 0.5,
                                                            double minSec = 0.3)
{
    std::vector<std::pair<double, double>> out;
    const double dur = std::min(before.durationSec, after.durationSec);
    if (dur <= 0.0 || before.hopSec <= 0.0) return out;
    auto spectralDiff = [&](double t) {
        if (before.spectrogramDb.empty() || after.spectrogramDb.empty() || before.frameSec <= 0.0 || after.frameSec <= 0.0)
            return 0.0f;
        const auto fa = static_cast<size_t>(std::clamp(t / before.frameSec, 0.0, static_cast<double>(before.spectrogramDb.size() - 1)));
        const auto fb = static_cast<size_t>(std::clamp(t / after.frameSec, 0.0, static_cast<double>(after.spectrogramDb.size() - 1)));
        const auto& a = before.spectrogramDb[fa];
        const auto& b = after.spectrogramDb[fb];
        const size_t n = std::min(a.size(), b.size());
        float sum = 0.0f;
        int used = 0;
        for (size_t k = 0; k < n; ++k)
            if (a[k] > -90.0f || b[k] > -90.0f) {
                sum += std::abs(a[k] - b[k]);
                ++used;
            }
        return used > 0 ? sum / static_cast<float>(used) : 0.0f;
    };
    bool in = false;
    double start = 0.0, lastChanged = 0.0;
    for (double t = 0.0; t < dur; t += before.hopSec) {
        const float a = TrackTimeline::at(before.shortTermLufs, before.hopSec, t);
        const float b = TrackTimeline::at(after.shortTermLufs, after.hopSec, t);
        const bool changed = (a > -60.0f || b > -60.0f) && (std::abs(a - b) > lu || spectralDiff(t) > db);
        if (changed) {
            if (!in) {
                if (!out.empty() && t - out.back().second < gapSec) {
                    start = out.back().first;
                    out.pop_back();
                } else {
                    start = t;
                }
                in = true;
            }
            lastChanged = t;
        } else if (in && t - lastChanged >= gapSec) {
            if (lastChanged + before.hopSec - start >= minSec) out.emplace_back(start, lastChanged + before.hopSec);
            in = false;
        }
    }
    if (in && lastChanged + before.hopSec - start >= minSec) out.emplace_back(start, std::min(dur, lastChanged + before.hopSec));
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
