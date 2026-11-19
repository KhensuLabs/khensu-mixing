// desktop-app/Source/ui/shared/Milestones.h
// Milestone Mix Sessions: the mix as ordered phases - Balance -> Frequency
// -> Space/Depth -> Polish - each with measurable gates evaluated from the
// current analysis. The issue queue's header shows where the mix stands
// (CON-1 moved the milestones there; the panel that drew them is gone).
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include <khensu/analysis/AnalysisResult.h>
#include <vector>

namespace khensu {

class IAppController;

namespace milestones {

struct Gate {
    juce::String label;
    bool met = false;
};

struct Phase {
    juce::String name;
    std::vector<Gate> gates;
    bool complete() const
    {
        for (const auto& g : gates)
            if (!g.met) return false;
        return !gates.empty();
    }
};

/// The four phases and their gates for `r`, against the session genre's
/// targets (defaults without one).
std::vector<Phase> evaluate(const AnalysisResult& r, IAppController& c);
/// Index of the first incomplete phase (phases.size() when all are met).
int currentPhase(const std::vector<Phase>& phases);
/// "Milestone 2 of 4: FREQUENCY - 1 of 2 checks met", or "All four
/// milestones met".
juce::String headline(const std::vector<Phase>& phases);

} // namespace milestones
} // namespace khensu
