#pragma once
// Loudness / true-peak delivery targets per playback intent (wiring M7).
// The Intake form collects CreativeTarget::playback_intent. The Loudness
// recommender uses it, opt-in (loudnessOverrideForIntent), to pick the
// integrated-loudness window and true-peak ceiling.
//
// Documented values:
//   streaming  -14 LUFS / -1.0 dBTP  (Spotify / YouTube / Tidal reference level)
//   club        -9 LUFS / -0.3 dBTP  (loud DJ-ready master; minimal headroom)
//   broadcast  -23 LUFS / -1.0 dBTP  (EBU R128 programme loudness)
//   headphone  -16 LUFS / -1.0 dBTP  (Apple Music / personal-listening level)
//   reference / empty / unknown -> no intent target (the genre window applies)
//
// THREAD_CONTEXT(AnyThread) - pure functions.
#include <optional>
#include <string>

namespace khensu {

struct PlaybackLoudnessTarget {
    std::string intent;           ///< canonical intent id, e.g. "club"
    float lufs = -14.0f;          ///< integrated loudness target
    float tolerance_lu = 1.0f;    ///< +/- window around lufs
    float true_peak_dbtp = -1.0f; ///< true-peak ceiling
    const char* standard = "";    ///< human-readable basis for the number
};

/// Target for a playback intent; nullopt for "reference", empty or unknown
/// intents (callers then fall back to the genre's loudness window).
inline std::optional<PlaybackLoudnessTarget> playbackIntentTarget(const std::string& intent)
{
    if (intent == "streaming") return PlaybackLoudnessTarget{intent, -14.0f, 1.0f, -1.0f, "streaming normalisation"};
    if (intent == "club") return PlaybackLoudnessTarget{intent, -9.0f, 1.0f, -0.3f, "club / DJ playback"};
    if (intent == "broadcast") return PlaybackLoudnessTarget{intent, -23.0f, 1.0f, -1.0f, "EBU R128 broadcast"};
    if (intent == "headphone") return PlaybackLoudnessTarget{intent, -16.0f, 1.0f, -1.0f, "headphone listening"};
    return std::nullopt;
}

/// The window that replaces the genre's in the loudness findings. Opt-in:
/// "streaming" is Intake's default, so it keeps the genre window (a loud
/// genre is not judged against -14 LUFS unless the user asks); club,
/// broadcast and headphone, picked on purpose, override it.
inline std::optional<PlaybackLoudnessTarget> loudnessOverrideForIntent(const std::string& intent)
{
    if (intent == "streaming") return std::nullopt;
    return playbackIntentTarget(intent);
}

} // namespace khensu
