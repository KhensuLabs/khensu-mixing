// plugin-copilot/Source/FixBatch.h
// ENH-10 (3): "Apply all safe fixes". Several findings' fixes mapped onto the
// built-in chain together (RecommendationDspMapper), one after another on a
// chained base: each later fix sees the EQ bands the earlier ones took, so
// every EQ move lands on its own band, and relative output-gain moves add up.
// A finding is skipped - with the reason - when it is not marked safe to
// apply automatically, when nothing in it maps onto the built-in chain (no
// free EQ band left included), or when it would overwrite a stage an earlier
// fix in the batch already set (two compressor settings, two ceilings).
// THREAD_CONTEXT(AnyThread) - pure function.
#pragma once

#include <JuceHeader.h>
#include <khensu/mixerchain/RecommendationDspMapper.h>
#include <khensu/recommendations/Recommendation.h>

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace khensu::copilot {

struct FixBatchResult {
    /// Plain values to write, by parameter id (the combined fix).
    std::map<std::string, float> values;
    /// The findings that made it in, in order.
    std::vector<std::string> appliedIds;
    /// One line per stage touched, prefixed with the finding.
    std::vector<std::string> notes;
    /// Keys of applied findings with no built-in equivalent ("<finding>: key").
    std::vector<std::string> unmappedKeys;
    struct Skipped {
        std::string id;
        juce::String what;   ///< the finding's title
        juce::String reason; ///< why it was left out
    };
    std::vector<Skipped> skipped;
    bool empty() const noexcept { return appliedIds.empty(); }
};

/// Maps `recs` in order at `amount` over `base` (plain values before any of
/// them). With `requireSafe`, findings not marked safe_to_auto_apply are
/// skipped.
FixBatchResult combineFixes(const std::vector<Recommendation>& recs, float amount,
                            const std::function<float(const std::string&)>& base, bool requireSafe);

/// STU-17: a generated mixer-chain recipe mapped onto the built-in chain
/// (mixerchain::mapRecipeToDsp), in Apply all's shape so the editor's apply
/// strip reports it the same way: appliedIds are the recipe plugins that set
/// something, notes one line per stage / band, and `skipped` every plugin
/// (or EQ move) that could not land, with the reason.
FixBatchResult recipeBatch(const mixerchain::RecipeDspMapping& mapping);

} // namespace khensu::copilot
