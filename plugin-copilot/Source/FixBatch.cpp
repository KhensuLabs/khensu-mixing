#include "FixBatch.h"

namespace khensu::copilot {

namespace {
juce::String utf8(const std::string& s) { return juce::String(juce::CharPointer_UTF8(s.c_str())); }

/// Ids an earlier fix may move again: the output gain is a relative move,
/// so a second fix adds to the first instead of replacing it.
bool stacks(const std::string& id) { return id == "out_gain_db"; }
} // namespace

FixBatchResult combineFixes(const std::vector<Recommendation>& recs, float amount,
                            const std::function<float(const std::string&)>& base, bool requireSafe)
{
    FixBatchResult out;
    const auto chained = [&out, &base](const std::string& id) {
        const auto it = out.values.find(id);
        if (it != out.values.end()) return it->second;
        return base ? base(id) : 0.0f;
    };
    for (const auto& rec : recs) {
        const auto title = utf8(rec.what.empty() ? rec.id : rec.what);
        if (requireSafe && !rec.safe_to_auto_apply) {
            out.skipped.push_back(
                {rec.id, title, TRANS("needs your judgement (not marked safe to apply automatically)")});
            continue;
        }
        const auto mapped = mixerchain::mapRecommendationToDsp(rec, amount, chained);
        if (mapped.empty()) {
            juce::StringArray keys;
            for (const auto& key : mapped.unmappedKeys)
                keys.add(utf8(key));
            out.skipped.push_back(
                {rec.id, title,
                 keys.isEmpty() ? TRANS("has no parameter targets")
                                : TRANS("the plugin's DSP cannot render it") + " (" + keys.joinIntoString(", ") + ")"});
            continue;
        }
        juce::String clash;
        for (const auto& [id, value] : mapped.values) {
            juce::ignoreUnused(value);
            if (!stacks(id) && out.values.count(id) != 0) {
                clash = utf8(id);
                break;
            }
        }
        if (clash.isNotEmpty()) {
            out.skipped.push_back(
                {rec.id, title, TRANS("would overwrite an earlier fix in this batch") + " (" + clash + ")"});
            continue;
        }
        for (const auto& [id, value] : mapped.values)
            out.values[id] = value;
        out.appliedIds.push_back(rec.id);
        for (const auto& note : mapped.notes)
            out.notes.push_back((rec.what.empty() ? rec.id : rec.what) + ": " + note);
        for (const auto& key : mapped.unmappedKeys)
            out.unmappedKeys.push_back((rec.what.empty() ? rec.id : rec.what) + ": " + key);
    }
    return out;
}

FixBatchResult recipeBatch(const mixerchain::RecipeDspMapping& mapping)
{
    using Reason = mixerchain::RecipeSkipReason;
    FixBatchResult out;
    out.values = mapping.values;
    out.appliedIds = mapping.mappedPlugins;
    out.notes = mapping.notes;
    for (const auto& s : mapping.skipped) {
        const auto detail = utf8(s.detail);
        juce::String reason;
        switch (s.reason) {
        case Reason::NoBuiltInStage:
            reason = TRANS("the built-in chain has no stage for it");
            break;
        case Reason::NoUsableSettings:
            reason = TRANS("no setting in its description could be read");
            break;
        case Reason::NoFreeEqBand:
            reason = TRANS("all six EQ bands hold larger moves");
            break;
        case Reason::StageTaken:
            reason = TRANS("an earlier plugin in the chain already set that stage");
            break;
        }
        if (detail.isNotEmpty()) reason << " (" << detail << ")";
        out.skipped.push_back({s.plugin, utf8(s.plugin), reason});
    }
    return out;
}

} // namespace khensu::copilot
