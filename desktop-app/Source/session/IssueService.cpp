// desktop-app/Source/session/IssueService.cpp
#include "IssueService.h"
#include <algorithm>
#include "../analysis/FixVerification.h"
#include "../services/IAnalysisHub.h"
#include "../services/IRenders.h"
#include "../services/IServiceLifecycle.h"
#include "RecommendationChain.h"
#include "RenderHistory.h"
#include "SessionService.h"
#include <khensu/analysis/AnalysisResult.h>
#include <khensu/feedback/LearnedPreferences.h>
#include <khensu/session/SessionManager.h>

namespace khensu {

namespace {
IIssues::Check checkOf(fix_verification::Outcome o) noexcept
{
    switch (o) {
    case fix_verification::Outcome::Resolved:
        return IIssues::Check::Resolved;
    case fix_verification::Outcome::Improved:
        return IIssues::Check::Improved;
    case fix_verification::Outcome::Unchanged:
        return IIssues::Check::Unchanged;
    case fix_verification::Outcome::Worse:
        return IIssues::Check::Worse;
    case fix_verification::Outcome::New:
        return IIssues::Check::New;
    case fix_verification::Outcome::NotChecked:
        return IIssues::Check::NotChecked;
    }
    return IIssues::Check::None;
}
} // namespace

IssueService::IssueService(IAnalysisHub& analysis, IRenders& renders, SessionService& sessions,
                           IServiceLifecycle& lifecycle)
    : analysis_(analysis), renders_(renders), sessions_(sessions), lifecycle_(lifecycle)
{
}

IssueService::~IssueService()
{
    waitForPendingWrites(); // a click made just before quitting still lands
    if (!attached_) return;
    analysis_.removeAnalysisListener(this);
    analysis_.removeVerificationListener(this);
    renders_.removeRenderTreeListener(this);
}

void IssueService::attach()
{
    if (attached_) return;
    attached_ = true;
    analysis_.addAnalysisListener(this, [this](const AnalysisResult&) {
        reloadStates();
        notify();
    });
    analysis_.addVerificationListener(this, [this] { notify(); });
    renders_.addRenderTreeListener(this, [this] { onRenderHistoryChanged(); });
}

void IssueService::resetForSession()
{
    // A fix of the session being left may still be rendering. RenderService
    // cancels it (its callback then arrives with ok = false), but nothing
    // here may depend on that: the generation makes a late completion a
    // no-op, so it cannot mark an issue of the next session done.
    ++generation_;
    fixInFlight_ = false;
    fixRenders_.clear();
    rows_.clear();
    notify();
}

IIssues::State IssueService::stateOf(const FeedbackRow& row) noexcept { return issue_state::of(row); }

std::string IssueService::dismissNote(const juce::String& reason)
{
    return issue_state::dismissNote(reason.trim().toStdString());
}

void IssueService::waitForPendingWrites()
{
    // Bounded: a write is one SQLite upsert plus an outbox insert, and the
    // store's busy timeout caps a locked database.
    const auto deadline = juce::Time::getMillisecondCounter() + 10000;
    while (writer_.getNumJobs() > 0 && juce::Time::getMillisecondCounter() < deadline)
        juce::Thread::sleep(1);
    if (writer_.getNumJobs() > 0)
        juce::Logger::writeToLog("Issue queue: gave up waiting for " + juce::String(writer_.getNumJobs()) +
                                 " pending feedback write(s); the store may lag the queue");
}

void IssueService::reloadStates()
{
    waitForPendingWrites(); // read what the last click wrote, not what was there before
    rows_.clear();
    if (!analysis_.hasAnalysis()) return;
    auto* fm = analysis_.getFeedbackManager();
    if (fm == nullptr) return;
    std::vector<std::string> ids;
    for (const auto& rec : analysis_.currentAnalysis().recommendations)
        if (rec.type != RecommendationType::Strength) ids.push_back(rec.id);
    rows_ = fm->getFeedbackBatch(ids);
}

std::vector<IIssues::Issue> IssueService::list() const
{
    std::vector<Issue> out;
    if (!analysis_.hasAnalysis()) return out;
    const auto& result = analysis_.currentAnalysis();
    const auto* check = analysis_.currentVerification();

    for (const auto& rec : result.recommendations) {
        if (rec.type == RecommendationType::Strength) continue;
        Issue issue;
        issue.recId = rec.id;
        issue.code = issueCodeOf(rec, result.file_id);
        issue.rec = rec;
        issue.renderable = hasRenderableChain(rec);
        if (auto it = rows_.find(rec.id); it != rows_.end()) issue.state = stateOf(it->second);
        if (check != nullptr)
            if (const auto* c = check->find(issue.code)) issue.check = checkOf(c->outcome);
        out.push_back(std::move(issue));
    }
    // Findings only the current version has: open, nothing to render yet -
    // the next analysis turns them into full findings. "New" when the
    // original could have raised them; "could not check" when it lacked the
    // input they need (FIX-1) - still a real finding of this version.
    if (check != nullptr)
        for (const auto& c : check->findings) {
            if (!c.onlyAfter) continue;
            Issue issue;
            issue.code = c.code;
            // The checked version's own recommendation (its advice and Why?),
            // under the code as its id - no recId: nothing to store until an
            // analysis adopts the finding.
            issue.rec = c.recAfter;
            issue.rec.id = c.code;
            if (issue.rec.what.empty()) issue.rec.what = c.title;
            issue.rec.severity = c.severityAfter;
            issue.check = checkOf(c.outcome);
            out.push_back(std::move(issue));
        }

    std::stable_sort(out.begin(), out.end(), [](const Issue& a, const Issue& b) {
        if (a.settled() != b.settled()) return !a.settled();
        return a.rec.severity > b.rec.severity;
    });
    return out;
}

int IssueService::openCount() const
{
    int n = 0;
    for (const auto& i : list())
        if (!i.settled()) ++n;
    return n;
}

std::optional<IIssues::Issue> IssueService::issue(const std::string& recId) const
{
    for (auto& i : list())
        if (i.recId == recId) return i;
    return std::nullopt;
}

void IssueService::fix(const std::string& recId, Done onDone)
{
    fixScaled(recId, 1.0f, std::move(onDone));
}

void IssueService::refine(const std::string& recId, bool stronger, Done onDone)
{
    if (fixInFlight_) {
        if (onDone) onDone(false, TRANS("Another fix is still rendering. Try again when it finishes."));
        return;
    }
    // Gentler: back to the version the fix was rendered from, then half the
    // move; Stronger: half the move again on top of the current version.
    if (!stronger) {
        if (!renders_.canUndoRender()) {
            if (onDone) onDone(false, TRANS("There is no earlier version to make the fix gentler from."));
            return;
        }
        renders_.undoRender();
    }
    fixScaled(recId, 0.5f, std::move(onDone));
}

void IssueService::fixScaled(const std::string& recId, float amount, Done onDone)
{
    // One fix at a time (FIX-4). Two at once process the same input, so only
    // the later render would be the current version while both issues were
    // recorded Done - and F pressed twice rendered the same fix twice.
    if (fixInFlight_) {
        if (onDone) onDone(false, TRANS("Another fix is still rendering. Try again when it finishes."));
        return;
    }
    if (!analysis_.hasAnalysis()) {
        if (onDone) onDone(false, TRANS("Analyze the session first."));
        return;
    }
    const Recommendation* rec = nullptr;
    for (const auto& r : analysis_.currentAnalysis().recommendations)
        if (r.id == recId) rec = &r;
    if (rec == nullptr) {
        if (onDone) onDone(false, TRANS("That finding is no longer in the analysis."));
        return;
    }
    fixInFlight_ = true; // before the call: a refusal answers synchronously
    juce::WeakReference<IssueService> self(this);
    renders_.applyRecommendationChain(
        amount == 1.0f ? *rec : scaledRecommendation(*rec, amount), [self, recId, onDone, generation = generation_](bool ok, juce::String pathOrError) {
            auto* s = self.get();
            if (s != nullptr && s->generation_ != generation) {
                // The session changed underneath (REF-2): nothing to record.
                if (onDone) onDone(false, TRANS("The session changed before the fix finished."));
                return;
            }
            if (s != nullptr) {
                s->fixInFlight_ = false;
                if (ok) {
                    // The render is now the current version (and the A/B's B
                    // side): a kept fix. The check (FEAT-1) says whether it
                    // worked.
                    s->fixRenders_[pathOrError] = FixRender{recId, true};
                    s->record(recId, true, /*learn*/ true, "Fixed via the issue queue (rendered)");
                }
            }
            if (onDone) onDone(ok, pathOrError);
        });
}

bool IssueService::fixTopIssue(Done onDone)
{
    if (fixInFlight_) { // handled: fix() says why
        fix({}, std::move(onDone));
        return true;
    }
    for (const auto& i : list())
        if (!i.settled() && i.renderable && !i.recId.empty()) {
            fix(i.recId, std::move(onDone));
            return true;
        }
    return false;
}

void IssueService::markDone(const std::string& recId)
{
    record(recId, true, /*learn*/ false, issue_state::kMarkedDoneNote);
}

void IssueService::dismiss(const std::string& recId, const juce::String& reason)
{
    record(recId, false, /*learn*/ true, dismissNote(reason));
}

void IssueService::reopen(const std::string& recId)
{
    record(recId, false, /*learn*/ false, issue_state::kReopenedNote);
}

void IssueService::verify()
{
    analysis_.verifyRender(renders_.currentRenderPath());
}

void IssueService::record(const std::string& recId, bool actedOn, bool learn, const std::string& note)
{
    if (recId.empty()) return;
    // Learn only from a change of mind, so the same choice twice doesn't
    // count as two. rows_ holds what the store holds for this analysis's
    // findings (reloadStates), so it answers without a read.
    const auto previous = rows_.find(recId);
    const bool changed = previous == rows_.end() || previous->second.acted_on != actedOn;
    rows_[recId] = FeedbackRow{actedOn, note};

    // FIX-6: the store and the outbox are written on the writer thread, in
    // click order; the queue's state above is already current, so the UI
    // never waits on SQLite.
    const juce::String sessionId = sessions_.activeSessionId();
    std::string feedbackJson, eventJson;
    juce::String key;
    auto sessionMgr = sessions_.sharedManager();
    if (sessionMgr) {
        // The feedback row above and these outbox rows are separate commits;
        // the outbox pair itself is atomic (AUDIT-056). session_id lets
        // deleting the session erase a still-queued mirror.
        auto* feedback = new juce::DynamicObject();
        feedback->setProperty("session_id", sessionId);
        feedback->setProperty("rec_id", juce::String(recId));
        feedback->setProperty("acted_on", actedOn);
        feedback->setProperty("note", juce::String(note));
        feedbackJson = juce::JSON::toString(juce::var(feedback), false).toStdString();

        const juce::String eventType = actedOn ? "recommendation_applied" : "recommendation_dismissed";
        auto* event = new juce::DynamicObject();
        event->setProperty("session_id", sessionId);
        event->setProperty("event_type", eventType);
        event->setProperty("summary", juce::String(note));
        event->setProperty("rec_id", juce::String(recId));
        eventJson = juce::JSON::toString(juce::var(event), false).toStdString();
        key = juce::Uuid().toString();
    }

    juce::WeakReference<IssueService> self(this);
    writer_.addJob([self, fm = analysis_.getFeedbackManager(), recId, actedOn, note, learn, changed, sessionMgr,
                    feedbackJson, eventJson, key] {
        // The store reports a failed write as a Result, not an exception:
        // log it, and learn / mirror only what was actually stored.
        const auto fail = [&recId](const std::string& what, const std::string& why) {
            juce::Logger::writeToLog("Issue queue: could not store " + juce::String(recId) + " (" +
                                     juce::String(what) + "): " + juce::String(why));
        };
        try {
            if (fm != nullptr) {
                if (const auto stored = fm->recordActedOn(recId, actedOn, note); stored.is_err()) {
                    fail("feedback", stored.error());
                    return;
                }
                if (learn && changed) learned_preferences::learnFromRecommendation(*fm, recId, actedOn);
            }
            if (sessionMgr) {
                if (const auto queued = sessionMgr->enqueueOutboxEvents(
                        {{(key + "-feedback").toStdString(), "feedback", feedbackJson},
                         {(key + "-event").toStdString(), "event", eventJson}});
                    queued.is_err()) {
                    fail("outbox", queued.error());
                    return;
                }
                juce::MessageManager::callAsync([self] {
                    if (self != nullptr) self->lifecycle_.dispatchOutboxIfNeeded();
                });
            }
        } catch (const std::exception& e) {
            fail("exception", e.what());
        }
    });
    notify();
}

void IssueService::onRenderHistoryChanged()
{
    // A fix is kept while the current version is built on its render - an
    // ancestor in the render tree (FIX-3), not merely earlier in the list.
    // "Start from original" followed by another fix leaves the first fix out
    // of the audio, so its issue reopens and taste learning hears it.
    const auto& tree = renders_.renderTree();
    const int current = renders_.currentRenderTreeNode();
    // Decide first, record after: record() notifies, and a listener may start
    // a fix, which adds to fixRenders_.
    std::vector<std::pair<std::string, bool>> changed;
    for (auto it = fixRenders_.begin(); it != fixRenders_.end();) {
        const auto state = RenderHistory::keptInTree(tree, current, it->first);
        if (state == RenderHistory::Kept::Gone) { // the history was cleared: no signal either way
            it = fixRenders_.erase(it);
            continue;
        }
        const bool kept = state == RenderHistory::Kept::Kept;
        if (kept != it->second.kept) {
            it->second.kept = kept;
            changed.emplace_back(it->second.recId, kept);
        }
        ++it;
    }
    for (const auto& [recId, kept] : changed)
        record(recId, kept, /*learn*/ true,
               kept ? "Fix render is in the current version again" : "Fix render is not in the current version");
}

void IssueService::notify()
{
    const auto listeners = listeners_; // a listener may deregister itself
    for (const auto& [key, cb] : listeners)
        if (cb && listeners_.count(key) != 0) cb();
}

} // namespace khensu
