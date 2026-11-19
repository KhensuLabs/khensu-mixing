#pragma once
// THREAD_CONTEXT(any) -- pure functions, no I/O.
#include <khensu/feedback/FeedbackManager.h>
#include <string>

namespace khensu {

/// How a finding's state is stored in the feedback store, shared by every
/// surface that reads or writes it: the desktop issue queue (CON-1,
/// `IssueService`) and the plugin's Suggestions tab. One row per
/// recommendation id: acted_on true is done; acted_on false with a note that
/// starts "dismissed" is dismissed; anything else (no row, "reopened") is open.
namespace issue_state {

enum class State { Open, Done, Dismissed };

inline constexpr const char* kDismissedPrefix = "dismissed";
/// Notes the stores keep for the other transitions. Only the dismissed
/// prefix carries meaning; these say where a change came from.
inline constexpr const char* kReopenedNote = "reopened";
inline constexpr const char* kMarkedDoneNote = "Marked done (nothing rendered)";
inline constexpr const char* kPluginDoneNote = "Marked done in the plugin";

inline State of(const FeedbackRow& row) noexcept
{
    if (row.acted_on) return State::Done;
    if (row.note.rfind(kDismissedPrefix, 0) == 0) return State::Dismissed;
    return State::Open;
}

/// "dismissed", or "dismissed: <reason>" (reason already trimmed).
inline std::string dismissNote(const std::string& reason)
{
    return reason.empty() ? std::string(kDismissedPrefix) : std::string(kDismissedPrefix) + ": " + reason;
}

/// The plugin's single toggle: an open finding becomes done; a done or
/// dismissed one is reopened, as the issue queue's Reopen does.
inline State toggled(State s) noexcept { return s == State::Open ? State::Done : State::Open; }

/// The row the plugin stores for `s` (dismissing is the desktop's job).
inline FeedbackRow rowFor(State s)
{
    switch (s) {
    case State::Done:
        return {true, kPluginDoneNote};
    case State::Dismissed:
        return {false, dismissNote({})};
    case State::Open:
    default:
        return {false, kReopenedNote};
    }
}

} // namespace issue_state
} // namespace khensu
