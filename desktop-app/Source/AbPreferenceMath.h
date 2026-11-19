// desktop-app/Source/AbPreferenceMath.h
#pragma once
#include <JuceHeader.h>

namespace khensu {

/// Pure step-toward-the-picked-side nudge math behind
/// AppController::learnFromAbPreference() - same learning-rate philosophy as
/// UserPreferenceModel::learnFromAction, just driven by an explicit A/B
/// choice instead of an accept/reject. Header-only free function (same
/// treatment as RenderJobManager.h's evaluateGuardrailAlert(), but inline
/// rather than declared+defined in a .cpp since AppController.cpp pulls in
/// AppController's full dependency graph - PythonServiceManager, SQLite
/// session state, etc. - which KhensuTests deliberately doesn't link) so
/// it's unit-testable without a live AppController (feedbackMgr/
/// sessionState_/database) - TODO.md "desktop matched-level A/B regression
/// tests covering accept/reject". Always clamps to +/-4 dB regardless of how
/// far the implied offset sits from currentOffsetDb.
inline float nudgeLufsOffsetTowardAbPreference(float currentOffsetDb, float pickedLufs, float genreTargetLufs,
                                               float stepDb = 0.3f) noexcept
{
    const float impliedOffset = pickedLufs - genreTargetLufs;
    const float nudged =
        currentOffsetDb + stepDb * (impliedOffset > currentOffsetDb ? 1.0f : (impliedOffset < currentOffsetDb ? -1.0f : 0.0f));
    return juce::jlimit(-4.0f, 4.0f, nudged);
}

/// DSK-07: what a matched A/B pick may teach.
struct AbPreferenceDecision {
    /// Never true: playback is loudness-matched (B attenuated to A, muted
    /// until measured), so the pick carries no loudness preference. The
    /// LUFS nudge above is kept only for its existing callers/tests.
    bool learnLufs = false;
    /// Record "processed vs original at equal loudness".
    bool recordProcessingPreference = false;
};

inline AbPreferenceDecision decideAbPreferenceLearning(bool hasProcessedB, bool bIsBlindReference) noexcept
{
    AbPreferenceDecision d;
    // A blind third-party reference is someone else's master: preferring it
    // says nothing about the user's own processing.
    d.recordProcessingPreference = hasProcessedB && !bIsBlindReference;
    return d;
}

/// FeedbackManager preference keys for the processing preference.
inline constexpr const char* kAbProcessingPreferenceKey = "ab_processing_preference"; ///< -1 original .. +1 processed
inline constexpr const char* kAbProcessingPicksKey = "ab_processing_picks";

/// Exponential moving step toward +1 (processed won) or -1 (original won).
inline float nudgeProcessingPreference(float current, bool preferredProcessed, float rate = 0.2f) noexcept
{
    const float target = preferredProcessed ? 1.0f : -1.0f;
    return juce::jlimit(-1.0f, 1.0f, current + rate * (target - current));
}

} // namespace khensu
