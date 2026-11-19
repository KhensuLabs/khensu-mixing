#pragma once
// THREAD_CONTEXT(WorkerThread|UIThread) -- performs SQLite I/O via FeedbackManager.
#include <khensu/feedback/FeedbackManager.h>
#include <string>
#include <vector>

namespace khensu {

/// One learned taste offset, described for the "What Khensu has learned" view.
///
/// What is learned (and only this): UserPreferenceModel's four global offsets
/// in recommendation_prefs - loudness (LUFS), dynamics (LRA), stereo width and
/// low-end weight. They move when you accept or reject the matching
/// recommendation types (lufs_too_loud, stereo_too_wide, ...) and when you pick
/// a side in an A/B comparison, and they shift the genre targets
/// (UserPreferenceModel::adapt) that recommendations, the mixer-chain
/// generator and the mastering agent measure against.
struct LearnedPreference {
    std::string pref_key;     // recommendation_prefs key, e.g. "pref_lufs_offset"
    std::string title;        // "Loudness"
    float offset = 0.0f;      // current offset from the genre default
    std::string offset_text;  // "+0.8 dB louder"
    int accepts = 0;          // accepted recommendations that moved it
    int rejects = 0;          // rejected recommendations that moved it
    int ab_picks = 0;         // A/B comparisons that moved it
    std::string last_updated; // ISO-8601 of the newest evidence, or empty
    std::string explanation;  // plain-language sentence for the UI
};

namespace learned_preferences {

/// The four learned keys, in display order.
const std::vector<std::string>& keys();

/// Every offset that has moved away from neutral or has logged evidence.
/// Empty = nothing learned yet.
std::vector<LearnedPreference> describe(FeedbackManager& fm);

/// Accept/reject of one recommendation: runs UserPreferenceModel::
/// learnFromAction, saves, and logs one evidence row per offset that moved.
/// Returns true if any offset changed.
bool learnFromRecommendation(FeedbackManager& fm, const std::string& recommendation_id, bool accepted);

/// Logs an A/B pick that moved the loudness offset from `before` to `after`
/// (the caller already saved the new value).
void recordAbPick(FeedbackManager& fm, bool preferredProcessed, float before, float after);

/// Forget one learned offset (value and evidence), or all four.
Result<void> reset(FeedbackManager& fm, const std::string& pref_key);
Result<void> resetAll(FeedbackManager& fm);

} // namespace learned_preferences
} // namespace khensu
