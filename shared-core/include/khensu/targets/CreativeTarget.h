#pragma once
#include <string>
#include <vector>

namespace khensu {

/// The user's creative, emotional, and technical intent for a session.
/// Every field is optional - empty string means "not specified".
/// The TargetInterpolator (SP-07) uses populated fields to derive recommendation thresholds.
/// THREAD_CONTEXT(AnyThread) - plain data struct.
struct CreativeTarget {
    // --- Genre & Style ---
    std::string primary_genre; // e.g. "trap", "shoegaze", "pop"
    std::string sub_genre;     // e.g. "melodic trap", "dream pop"
    std::string genre_blend;   // e.g. "trap + shoegaze"

    // --- Mood & Emotion ---
    std::string mood; // e.g. "melancholic", "euphoric", "aggressive"

    // --- Energy & Tempo ---
    float energy = 0.0f; // 0-100
    float bpm = 0.0f;    // 0 = not specified / auto-detect

    // --- Intent ---
    std::string playback_intent; // "streaming"|"club"|"broadcast"|"headphone"|"reference"

    // --- Musical Psychology Targets ---
    float hook_emphasis = 50.0f;       // 0-100: how prominent hooks should be
    float chorus_payoff = 50.0f;       // 0-100: desired verse→chorus contrast
    float heaviness_target = 50.0f;    // 0-100: overall perceptual heaviness goal
    float memorability_target = 50.0f; // 0-100: earworm/salience priority

    // --- Creative Notes ---
    /// Free text: "sounds like a bedroom at 3am in summer". Shared with the
    /// copilot as part of the session target (WF-11).
    std::string vision_statement;

    // --- Session control (SP-16A) ---
    // WF-11 removed the fields nothing set or read (era, emotion, vocal intent,
    // mix / mastering philosophy, AI / humanizer flags, analysis stage, known
    // issues, destructive-suggestion switch); sessions that stored them load fine.
    float confidence_threshold = 0.0f;              ///< Hide recs below this confidence; 0 = show all
    bool run_deep_analysis = true;                  ///< If false, skip advanced python analysis
    std::vector<std::string> reference_track_paths; ///< Paths to reference files (populated by SP-16D)

    // --- Genre confirmation (P0-1, khensu/targets/GenreConfirmation.h) ---
    /// False when the user turned genre-relative targets off at intake.
    bool use_genre_targets = true;
    /// How the user confirmed the genre the genre-relative actions use:
    /// "user_stated", "user_confirmed_candidate" or "reference_track";
    /// empty = not confirmed (a detected candidate alone never counts).
    std::string genre_confirmed_source;

    bool operator==(const CreativeTarget& o) const noexcept
    {
        return primary_genre == o.primary_genre && sub_genre == o.sub_genre && mood == o.mood &&
               energy == o.energy && bpm == o.bpm && playback_intent == o.playback_intent;
    }
};

} // namespace khensu
