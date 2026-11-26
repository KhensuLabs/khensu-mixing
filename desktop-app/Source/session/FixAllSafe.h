// desktop-app/Source/session/FixAllSafe.h
// ENH-2 "Fix all safe": one chain built from every open finding whose fix is
// validated, rendered once as one version (through the guarded apply-chain
// job), then checked (FEAT-1). Pure and unit-tested.
//
// "Validated" is the bar a fix must clear on the measured corpus
// (resources/calibration/finding_evidence.json, MEAS-4): the app's own Fix
// cleared the defect on at least kMinClearsRate of at least kMinTried tracks.
// On top of that it is a Fix top issue candidate (IIssues::Issue::
// topFixCandidate: open, rendered by Fix, not the denoise RUN-6 keeps one
// press away) and its chain has no stage that needs listening (denoise).
// Advice-only findings and findings never measured are left for the user.
#pragma once
#include <JuceHeader.h>
#include <map>
#include <string>
#include <utility>
#include <vector>
#include <khensu/ipc/HttpServiceBridge.h>
#include "../services/IIssues.h"
#include "../ui/shared/FindingEvidence.h"

namespace khensu::fix_all_safe {

constexpr int kMinTried = 10;
constexpr double kMinClearsRate = 0.7;

using Evidence = std::map<juce::String, finding_evidence::Entry>;
using Chain = std::vector<ipc::HttpServiceBridge::EffectSpec>;

/// Why `issue` is not part of Fix all safe, or "" when it is.
juce::String whyNotSafe(const IIssues::Issue& issue, const Evidence& evidence);

struct Plan {
    std::vector<std::string> recIds; ///< the findings the chain fixes, most severe first
    std::vector<std::string> codes;  ///< their issue codes (same order)
    Chain chain;                     ///< one chain, in processing order
    /// Open findings left out, with the reason (for the tooltip / status).
    std::vector<std::pair<std::string, juce::String>> skipped;
    bool empty() const noexcept { return recIds.empty() || chain.empty(); }
};

/// The plan for `issues` (IIssues::list() order). Each finding contributes
/// its buildRecommendationChain(); the stages are merged into one processing
/// order: high-pass, EQ, dynamics, colour, width, gain, limiter. Duplicates
/// collapse - one high-pass at the highest cutoff, the gains summed into one
/// stage, one limiter at the lowest ceiling - so two findings never stack the
/// same gain change or limit twice.
Plan plan(const std::vector<IIssues::Issue>& issues, const Evidence& evidence);

/// Merges per-finding chains into one (exposed for tests).
Chain mergeChains(const std::vector<Chain>& chains);

/// The processing rank of an effect (lower first).
int stageRank(const juce::String& effect);

} // namespace khensu::fix_all_safe
