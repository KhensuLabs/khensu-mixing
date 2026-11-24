// desktop-app/Source/session/IssueService.h
#pragma once
#include <JuceHeader.h>
#include <functional>
#include <map>
#include <string>
#include <khensu/feedback/FeedbackManager.h>
#include "../services/IIssues.h"

namespace khensu {

class IAnalysisHub;
class IRenders;
class IServiceLifecycle;
class SessionService;

/// The one issue queue (CON-1), behind IIssues. Reads the findings of the
/// published analysis and the latest fix check (FEAT-1); stores each
/// finding's state in the feedback store (acted_on + note: done = acted on,
/// dismissed = not acted on with a "dismissed" note, no row = open) and
/// mirrors it to the service through the outbox; learns taste only from a
/// change of mind, and from fix renders the user keeps or undoes.
///
/// Message thread. attach() registers with the analysis and render hubs (from
/// the owner's constructor body, ENG-2); the destructor deregisters.
class IssueService final : public IIssues {
  public:
    IssueService(IAnalysisHub& analysis, IRenders& renders, SessionService& sessions, IServiceLifecycle& lifecycle);
    ~IssueService() override;

    void attach();
    /// Forgets the session being left (REF-2): its stored states, its fix
    /// renders and a fix still rendering - whose late completion is then
    /// ignored. Called from AppController::resetSessionScopedState().
    void resetForSession();

    // ---- IIssues ----
    std::vector<Issue> list() const override;
    int openCount() const override;
    std::optional<Issue> issue(const std::string& recId) const override;
    void fix(const std::string& recId, Done onDone = {}) override;
    void fixScaled(const std::string& recId, float amount, Done onDone = {}) override;
    void fixInRegion(const std::string& recId, double startSec, double endSec, const juce::String& label,
                     Done onDone = {}) override;
    bool fixTopIssue(Done onDone = {}) override;
    void refine(const std::string& recId, bool stronger, Done onDone = {}) override;
    bool fixInProgress() const override { return fixInFlight_; }
    void markDone(const std::string& recId) override;
    void dismiss(const std::string& recId, const juce::String& reason = {}) override;
    void reopen(const std::string& recId) override;
    void verify() override;
    void addIssuesListener(void* key, std::function<void()> cb) override { listeners_[key] = std::move(cb); }
    void removeIssuesListener(void* key) override { listeners_.erase(key); }

    /// The stored state of a feedback row (pure; unit-tested).
    /// Blocks until every queued feedback-store write has landed (FIX-6).
    /// Tests call it before reading the store; the destructor and
    /// reloadStates() call it so nothing is lost or read stale.
    void waitForPendingWrites();

    static State stateOf(const FeedbackRow& row) noexcept;
    /// The note a dismissal stores ("dismissed" or "dismissed: <reason>").
    static std::string dismissNote(const juce::String& reason);

  private:
    void reloadStates();
    void notify();
    /// Upserts the feedback row, learns on a change of mind when `learn`,
    /// and mirrors the row + a provenance event through the outbox.
    void record(const std::string& recId, bool actedOn, bool learn, const std::string& note);
    void onRenderHistoryChanged();

    IAnalysisHub& analysis_;
    IRenders& renders_;
    SessionService& sessions_;
    IServiceLifecycle& lifecycle_;
    bool attached_ = false;
    bool fixInFlight_ = false; ///< a fix is rendering (FIX-4)
    int generation_ = 0;       ///< bumped by resetForSession(); a fix of an older one is dropped

    std::map<std::string, FeedbackRow> rows_; ///< by recommendation id, for the current analysis
    /// Fix renders and whether each is still part of the accepted audio
    /// (P0-2): keeping or undoing one is a taste signal.
    struct FixRender {
        std::string recId;
        bool kept = true;
        float amount = 1.0f; ///< the share of the move rendered (OVH-6, NEW-4)
    };
    std::map<juce::String, FixRender> fixRenders_;
    std::map<void*, std::function<void()>> listeners_;

    /// FIX-6: the feedback store and outbox writes run here, one at a time in
    /// click order, instead of on the message thread. Declared last: it is
    /// drained in the destructor before anything it touches goes.
    juce::ThreadPool writer_{1};

    JUCE_DECLARE_WEAK_REFERENCEABLE(IssueService)
    JUCE_DECLARE_NON_COPYABLE(IssueService)
};

} // namespace khensu
