// shared-core/include/khensu/targets/GenreConfirmation.h
#pragma once
#include <khensu/targets/CreativeTarget.h>
#include <string>

namespace khensu::genre_confirmation {

/// CreativeTarget::genre_confirmed_source values - the same strings as the
/// service's genre-context `selected_source` (analysis/genre_context.py).
inline constexpr const char* kUserStated = "user_stated";
inline constexpr const char* kConfirmedCandidate = "user_confirmed_candidate";
inline constexpr const char* kReferenceTrack = "reference_track";

/// What Intake's genre-context controls said when the session was created.
struct IntakeChoice {
    std::string selectionMode;   ///< "detected" | "stated" | "reference" | "none"
    bool useGenreTargets = true; ///< the "Allow genre-relative targets" toggle
    std::string statedGenreId;   ///< the Target panel's genre ("" = none)
    bool referenceExists = false;
    bool confirmDetected = false; ///< "confirm the detected candidate" toggle
};

/// The confirmation source for `choice`, mirroring genre_context.py's
/// selection: a stated genre, a named reference or a confirmed detected
/// candidate confirms; a detected candidate alone, or targets off, does not.
inline std::string sourceForIntake(const IntakeChoice& choice)
{
    if (!choice.useGenreTargets || choice.selectionMode == "none") return {};
    if (choice.selectionMode == "stated" && !choice.statedGenreId.empty()) return kUserStated;
    if (choice.selectionMode == "reference" && choice.referenceExists) return kReferenceTrack;
    if (choice.selectionMode == "detected" && choice.confirmDetected) return kConfirmedCandidate;
    if (choice.selectionMode == "detected" && !choice.statedGenreId.empty()) return kUserStated;
    if (choice.selectionMode == "detected" && choice.referenceExists) return kReferenceTrack;
    return {};
}

/// Whether genre-relative actions (the Mastering Agent, Section Intensity,
/// the chat's genre-relative chains) may run: targets are on, the genre is
/// confirmed by the user or a reference track is loaded, and the analysis
/// doesn't find the genre section-conflicted. The deep response can't decide
/// this - it is cached per file and knows nothing of the session.
inline bool actionAllowed(const CreativeTarget& target, bool referenceLoaded, bool genreIsHybrid)
{
    if (!target.use_genre_targets || genreIsHybrid) return false;
    return !target.genre_confirmed_source.empty() || referenceLoaded;
}

} // namespace khensu::genre_confirmation
