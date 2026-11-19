// desktop-app/Source/ui/shared/ScanSummary.h
// WF-14: a follow-up scan's result, cut down to what a reader (the copilot,
// the report) can use - its scalar fields and short lists - before it goes
// into the session's scan store (IAnalysisHub::recordScanResult). Heat-map
// grids and long time series are dropped: the chat context has a size budget
// and a model reads "mono score 42 on phone speakers", not 2,000 frames.
// Header-only so it is unit-testable.
#pragma once
#include <JuceHeader.h>

namespace khensu::scan_summary {

inline constexpr int kMaxListItems = 12;
inline constexpr int kMaxDepth = 4;

inline juce::var compact(const juce::var& v, int depth = 0)
{
    if (v.isArray()) {
        const auto* arr = v.getArray();
        if (arr == nullptr || arr->size() > kMaxListItems || depth >= kMaxDepth) return {};
        juce::Array<juce::var> out;
        for (const auto& item : *arr) {
            auto c = compact(item, depth + 1);
            if (!c.isVoid()) out.add(c);
        }
        return out.isEmpty() ? juce::var() : juce::var(out);
    }
    if (auto* obj = v.getDynamicObject()) {
        if (depth >= kMaxDepth) return {};
        juce::DynamicObject::Ptr out = new juce::DynamicObject();
        for (const auto& p : obj->getProperties()) {
            auto c = compact(p.value, depth + 1);
            if (!c.isVoid()) out->setProperty(p.name, c);
        }
        return out->getProperties().isEmpty() ? juce::var() : juce::var(out.get());
    }
    if (v.isString()) return v.toString().length() <= 200 ? v : juce::var(v.toString().substring(0, 200));
    return v; // numbers, bools; void stays void
}

} // namespace khensu::scan_summary
