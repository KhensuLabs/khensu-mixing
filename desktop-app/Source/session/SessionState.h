// desktop-app/Source/session/SessionState.h
#pragma once
#include <memory>
#include <optional>
#include <JuceHeader.h>
#include <khensu/session/Session.h>
#include <khensu/analysis/AnalysisResult.h>
#include <khensu/ipc/HttpServiceBridge.h> // ipc::StemSeparationResult

namespace khensu {

/// The state that belongs to "the session currently open" — extracted from
/// AppController (audit M4, SessionState slice). It bundles the Session object
/// with the per-session derived data that must be cleared together when the
/// session changes: the vocal-stem analysis, the last stem-separation result,
/// which stem is soloed, and whether a separation job is in flight.
///
/// This is plain state with no behaviour beyond bundling + reset. AppController
/// still owns lifecycle (creating/loading sessions, kicking off analysis and
/// separation) and exposes the same public accessors, now forwarding here, so
/// the ~57 panels that read the session through AppController are unchanged.
struct SessionState {
    std::unique_ptr<Session> session;
    std::optional<AnalysisResult> vocalAnalysis;
    std::optional<ipc::StemSeparationResult> lastSeparationResult;
    juce::String soloedStemName;  ///< "" = full mix is playing
    bool separatingStems = false; ///< a separation job is running

    bool hasSession() const noexcept { return session != nullptr; }

    /// Clear everything back to "no session open". Used when a session closes
    /// or a new one replaces it, so no derived state leaks across sessions.
    void reset() { *this = SessionState{}; }
};

} // namespace khensu
