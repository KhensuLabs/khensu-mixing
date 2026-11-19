// THREAD_CONTEXT(WorkerThread|UIThread) -- performs SQLite I/O via FeedbackManager.
#include <khensu/feedback/LearnedPreferences.h>
#include <khensu/targets/UserPreferenceModel.h>

#include <cmath>
#include <cstdio>
#include <map>
#include <utility>

namespace khensu::learned_preferences {

namespace {

constexpr const char* kLufs = "pref_lufs_offset";
constexpr const char* kLra = "pref_lra_offset";
constexpr const char* kWidth = "pref_stereo_width_offset";
constexpr const char* kLowEnd = "pref_low_end_weight_offset";

// Below this an offset reads as "back at the genre default".
constexpr float kNeutralEpsilon = 1.0e-3f;

/// The recommendation types UserPreferenceModel::learnFromAction reacts to,
/// the offset each one moves, and how to name it in a sentence.
struct RecType {
    const char* id;
    const char* prefKey;
    const char* one;  // "'too loud' warning"
    const char* many; // "'too loud' warnings"
};

constexpr RecType kRecTypes[] = {
    {"lufs_too_loud", kLufs, "'too loud' warning", "'too loud' warnings"},
    {"lufs_too_quiet", kLufs, "'too quiet' warning", "'too quiet' warnings"},
    {"lra_too_compressed", kLra, "'over-compressed' warning", "'over-compressed' warnings"},
    {"lra_too_dynamic", kLra, "'too dynamic' warning", "'too dynamic' warnings"},
    {"stereo_too_narrow", kWidth, "'too narrow' warning", "'too narrow' warnings"},
    {"stereo_too_wide", kWidth, "'too wide' warning", "'too wide' warnings"},
    {"low_end_heavy", kLowEnd, "'too much low end' warning", "'too much low end' warnings"},
    {"low_end_light", kLowEnd, "'not enough low end' warning", "'not enough low end' warnings"},
};

const RecType* recTypeFor(const std::string& recommendationId)
{
    for (const auto& t : kRecTypes)
        if (recommendationId.find(t.id) != std::string::npos) return &t;
    return nullptr;
}

std::string fmt(float v, int decimals)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.*f", decimals, static_cast<double>(v));
    return buf;
}

const char* titleFor(const std::string& key)
{
    if (key == kLufs) return "Loudness";
    if (key == kLra) return "Dynamics";
    if (key == kWidth) return "Stereo width";
    return "Low-end weight";
}

/// "+0.8 dB louder" style description of an offset.
std::string offsetText(const std::string& key, float offset)
{
    const float mag = std::abs(offset);
    if (mag < kNeutralEpsilon) return "at the genre default";
    const bool up = offset > 0.0f;
    if (key == kLufs) return fmt(mag, 1) + " dB " + (up ? "louder" : "quieter");
    if (key == kLra) return fmt(mag, 1) + " LU " + (up ? "more dynamic" : "more compressed");
    if (key == kWidth) return fmt(mag, 2) + (up ? " wider" : " narrower");
    // low_end_weight is a 0-1 share; show it in percentage points.
    return fmt(mag * 100.0f, 0) + (up ? " points more low end" : " points less low end");
}

/// What the offset does to recommendations, completing "so ...".
std::string effectText(const std::string& key, float offset)
{
    const std::string amount = offsetText(key, offset);
    if (std::abs(offset) < kNeutralEpsilon) {
        if (key == kLufs) return "loudness targets are back at the genre default";
        if (key == kLra) return "dynamics targets are back at the genre default";
        if (key == kWidth) return "stereo-width targets are back at the genre default";
        return "low-end targets are back at the genre default";
    }
    if (key == kLufs) return "loudness targets now sit " + amount + " than the genre default";
    if (key == kLra) return "the dynamic-range window is now " + amount + " than the genre default";
    if (key == kWidth) return "the stereo-width window is now " + amount + " than the genre default";
    return "the low-end target now allows " + amount + " than the genre default";
}

std::string plural(int n, const char* one, const char* many) { return std::to_string(n) + " " + (n == 1 ? one : many); }

float currentValue(FeedbackManager& fm, const std::string& key)
{
    auto r = fm.getPreference(key, 0.0f);
    return r.is_ok() ? r.value() : 0.0f;
}

} // namespace

const std::vector<std::string>& keys()
{
    static const std::vector<std::string> k{kLufs, kLra, kWidth, kLowEnd};
    return k;
}

std::vector<LearnedPreference> describe(FeedbackManager& fm)
{
    std::vector<PreferenceLearningEvent> events;
    if (auto r = fm.listLearningEvents(); r.is_ok()) events = std::move(r.value());

    std::vector<LearnedPreference> out;
    for (const auto& key : keys()) {
        LearnedPreference p;
        p.pref_key = key;
        p.title = titleFor(key);
        p.offset = currentValue(fm, key);
        p.offset_text = offsetText(key, p.offset);

        // (phrase, accepted) -> count, in first-seen order for a stable sentence.
        std::vector<std::pair<std::pair<std::string, bool>, int>> recCounts;
        int abProcessed = 0, abOriginal = 0;
        for (const auto& ev : events) {
            if (ev.pref_key != key) continue;
            if (ev.created_at > p.last_updated) p.last_updated = ev.created_at;
            if (ev.source == "ab_pick") {
                ++p.ab_picks;
                (ev.accepted ? abProcessed : abOriginal) += 1;
                continue;
            }
            (ev.accepted ? p.accepts : p.rejects) += 1;
            const auto* type = recTypeFor(ev.recommendation_id);
            const std::string phrase = type != nullptr ? type->many : "";
            const auto slot = std::make_pair(phrase, ev.accepted);
            bool found = false;
            for (auto& rc : recCounts)
                if (rc.first == slot) {
                    ++rc.second;
                    found = true;
                }
            if (!found) recCounts.emplace_back(slot, 1);
        }

        const bool hasEvidence = p.accepts + p.rejects + p.ab_picks > 0;
        if (!hasEvidence && std::abs(p.offset) < kNeutralEpsilon) continue;

        std::vector<std::string> causes;
        for (const auto& [slot, n] : recCounts) {
            std::string noun = n == 1 ? "related recommendation" : "related recommendations";
            for (const auto& t : kRecTypes)
                if (slot.first == t.many) noun = n == 1 ? t.one : t.many;
            causes.push_back(std::string(slot.second ? "accepted " : "rejected ") + std::to_string(n) + " " + noun);
        }
        if (abProcessed > 0)
            causes.push_back("picked the processed render in " + plural(abProcessed, "A/B comparison", "A/B comparisons"));
        if (abOriginal > 0)
            causes.push_back("picked the original in " + plural(abOriginal, "A/B comparison", "A/B comparisons"));

        std::string sentence;
        if (causes.empty()) {
            sentence = "Learned from earlier choices made before Khensu kept a history";
        } else {
            sentence = "You ";
            for (size_t i = 0; i < causes.size(); ++i) {
                if (i > 0) sentence += (i + 1 == causes.size()) ? " and " : ", ";
                sentence += causes[i];
            }
        }
        p.explanation = sentence + ", so " + effectText(key, p.offset) + ".";
        out.push_back(std::move(p));
    }
    return out;
}

bool learnFromRecommendation(FeedbackManager& fm, const std::string& recommendation_id, bool accepted)
{
    UserPreferenceModel model;
    model.loadFromDatabase(fm);
    const float before[] = {model.getLufsOffset(), model.getLraOffset(), model.getStereoWidthOffset(),
                            model.getLowEndWeightOffset()};
    if (!model.learnFromAction(recommendation_id, accepted)) return false;
    model.saveToDatabase(fm);
    const float after[] = {model.getLufsOffset(), model.getLraOffset(), model.getStereoWidthOffset(),
                           model.getLowEndWeightOffset()};
    for (size_t i = 0; i < keys().size(); ++i) {
        if (before[i] == after[i]) continue;
        PreferenceLearningEvent ev;
        ev.pref_key = keys()[i];
        ev.source = "recommendation";
        ev.recommendation_id = recommendation_id;
        ev.accepted = accepted;
        ev.value_before = before[i];
        ev.value_after = after[i];
        fm.recordLearningEvent(ev);
    }
    return true;
}

void recordAbPick(FeedbackManager& fm, bool preferredProcessed, float before, float after)
{
    if (before == after) return;
    PreferenceLearningEvent ev;
    ev.pref_key = kLufs;
    ev.source = "ab_pick";
    ev.accepted = preferredProcessed;
    ev.value_before = before;
    ev.value_after = after;
    fm.recordLearningEvent(ev);
}

Result<void> reset(FeedbackManager& fm, const std::string& pref_key)
{
    for (const auto& k : keys())
        if (k == pref_key) return fm.resetLearnedPreference(pref_key);
    return Result<void>::err("learned_preferences::reset: not a learned preference key: " + pref_key);
}

Result<void> resetAll(FeedbackManager& fm) { return fm.resetLearnedPreferences(keys()); }

} // namespace khensu::learned_preferences
