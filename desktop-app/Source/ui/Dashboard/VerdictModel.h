// desktop-app/Source/ui/Dashboard/VerdictModel.h
// OVH-1: one score and one list behind the Dashboard's headline.
//
// The Dashboard used to show three readiness numbers at once - "45%
// MASTER-READY" (MixReport's mastering readiness), "READINESS (heuristic) 79"
// and "MIX CRAFT 79/100" (both MixScorecard::overall_score) - plus a "MASTER
// READY /100" tile, each with its own colour thresholds. The verdict counted
// the stored recommendations while the scores used the derived list, and its
// "Start here" named the most severe finding while "Fix top issue" fixed the
// first renderable one. Everything the headline says now comes from here:
//  - the number is the scorecard's overall score ("Readiness"), explained by
//    its categories (the Mix Craft card is its breakdown);
//  - one grade table colours it and every category score;
//  - the counts come from the same recommendation list the score was built from;
//  - "Start here" picks the issue with the same rule the issue queue's Fix
//    top issue uses, so the button fixes the issue the card names.
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include <khensu/recommendations/Recommendation.h>
#include <khensu/recommendations/RecommendationType.h>
#include <string>
#include <vector>
#include "../../services/IIssues.h"
#include "../shared/KhensuTheme.h"

namespace khensu::verdict {

/// One grade table for the readiness score and its category scores. The
/// bounds match MixScorecard's bands: 80+ is "Near Target" / "Meets Targets",
/// under 50 is "Early Draft" / "Rough".
enum class Grade { Good, Fair, Poor };

inline Grade gradeOf(float score) noexcept
{
    if (score >= 80.0f) return Grade::Good;
    if (score >= 50.0f) return Grade::Fair;
    return Grade::Poor;
}

/// Severity colours carry the grade (green / amber / red) - never decoration.
inline juce::Colour colourOf(Grade g)
{
    switch (g) {
    case Grade::Good: return KhensuTheme::accentGreen;
    case Grade::Fair: return KhensuTheme::accentAmber;
    case Grade::Poor: return KhensuTheme::accentRed;
    }
    return KhensuTheme::textSecondary;
}

inline juce::Colour colourFor(float score) { return colourOf(gradeOf(score)); }

/// Severity buckets (the Mix Notes scheme): 66+ severe, 33+ moderate, else minor.
struct Counts {
    int severe = 0, moderate = 0, minor = 0, strengths = 0;
    int issues() const noexcept { return severe + moderate + minor; }
};

inline Counts countFindings(const std::vector<Recommendation>& recs)
{
    Counts c;
    for (const auto& rec : recs) {
        if (rec.type == RecommendationType::Strength)
            ++c.strengths;
        else if (rec.severity >= 66.0f)
            ++c.severe;
        else if (rec.severity >= 33.0f)
            ++c.moderate;
        else
            ++c.minor;
    }
    return c;
}

/// What "Start here" names and whether its button can fix it in one click.
struct StartHere {
    std::string recId;
    juce::String text;    ///< the finding, in its own words; empty = nothing open
    bool fixable = false; ///< the issue queue's Fix top issue fixes exactly this one
    /// RUN-7: the fix check raised it on the current version - the analysis
    /// on screen (the original) does not have it, so the card says so.
    bool onCurrentVersion = false;
};

/// RUN-8: whether the issue queue already reflects a checked version (some
/// issue carries a fix-check outcome) - then the queue's open count and the
/// analysis's counts describe different versions and the card names both.
inline bool queueReflectsCheck(const std::vector<IIssues::Issue>& issues)
{
    for (const auto& i : issues)
        if (i.check != IIssues::Check::None && i.check != IIssues::Check::NotChecked) return true;
    return false;
}

/// The issue queue lists open issues first, most severe first. Fix top issue
/// (IssueService::fixTopIssue, IssueQueuePanel::fixTopIssue) fixes the first
/// open, renderable one with a recommendation id - so that one is named here.
/// With no fixable issue, the first open one is named and nothing claims a fix.
inline StartHere startHere(const std::vector<IIssues::Issue>& issues)
{
    const auto describe = [](const IIssues::Issue& i) {
        return juce::String(juce::CharPointer_UTF8(i.rec.what.empty() ? i.rec.suggested_action.c_str()
                                                                       : i.rec.what.c_str()));
    };
    for (const auto& i : issues)
        if (i.topFixCandidate())
            return {i.recId, describe(i), true, i.check == IIssues::Check::New};
    for (const auto& i : issues)
        if (!i.settled()) return {i.recId, describe(i), false, i.check == IIssues::Check::New};
    return {};
}

} // namespace khensu::verdict
