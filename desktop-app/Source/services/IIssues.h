// desktop-app/Source/services/IIssues.h
#pragma once
#include <JuceHeader.h>
#include <functional>
#include <optional>
#include <string>
#include <vector>
#include <khensu/feedback/IssueState.h>
#include <khensu/recommendations/Recommendation.h>

namespace khensu {

/// One issue queue (CON-1): the findings of the session's analysis with one
/// state each - open, done or dismissed - and, once a fixed version has been
/// checked (FEAT-1), what the check found. Every surface that acts on a
/// finding (the Issues screen, the Dashboard's notes, Mix/Master's list, the
/// F key) goes through this, so a finding fixed in one place reads fixed
/// everywhere. State is stored per recommendation in the feedback store.
/// Message thread.
class IIssues {
  public:
    /// Stored the same way the plugin's Suggestions tab reads it (issue_state).
    using State = issue_state::State;
    /// What the latest check of the current version found for an issue.
    /// NotChecked: the check could not measure this finding (the analysis of
    /// one of the two versions lacked its input); it stays open.
    enum class Check { None, Resolved, Improved, Unchanged, Worse, New, NotChecked };

    struct Issue {
        std::string recId; ///< the recommendation id ("" for a finding only the check raised)
        std::string code;  ///< issue code, the same in every analysis of this session
        Recommendation rec; ///< the finding (for a check-only finding: what and severity)
        State state = State::Open;
        Check check = Check::None;
        bool renderable = false; ///< Fix renders it (it has concrete parameters)
        /// Done, dismissed or resolved: listed after the open issues.
        bool settled() const noexcept { return state != State::Open || check == Check::Resolved; }
    };

    using Done = std::function<void(bool ok, juce::String message)>;
    virtual ~IIssues() = default;

    /// Open issues first (most severe first), then the settled ones.
    virtual std::vector<Issue> list() const = 0;
    /// Open issues the latest check hasn't resolved.
    virtual int openCount() const = 0;
    /// The issue for recommendation `recId`, if it is in the queue.
    virtual std::optional<Issue> issue(const std::string& recId) const = 0;

    /// Renders the fix (from the current version, WF-1), marks the issue done
    /// and lets the check (FEAT-1) say whether it worked.
    virtual void fix(const std::string& recId, Done onDone = {}) = 0;
    /// Fixes the most severe open, renderable issue; false when there is none.
    virtual bool fixTopIssue(Done onDone = {}) = 0;
    /// True while a fix is rendering. One at a time (FIX-4): a fix asked for
    /// in that time renders nothing and says why through its onDone.
    virtual bool fixInProgress() const = 0;
    /// NEW-4: a fix the check marked Improved but not resolved, adjusted.
    /// Stronger renders half the move again on the current version; Gentler
    /// steps back to the version before it and renders half the move.
    virtual void refine(const std::string& recId, bool stronger, Done onDone = {}) = 0;
    virtual void markDone(const std::string& recId) = 0;
    /// `reason` is kept with the feedback row ("Too bright", ...).
    virtual void dismiss(const std::string& recId, const juce::String& reason = {}) = 0;
    /// Back to open (undoes done / dismissed).
    virtual void reopen(const std::string& recId) = 0;
    /// Re-checks the current version against the original (FEAT-1).
    virtual void verify() = 0;

    /// The queue changed: a new analysis, a state change or a check.
    virtual void addIssuesListener(void* key, std::function<void()> cb) = 0;
    virtual void removeIssuesListener(void* key) = 0;
};

} // namespace khensu
