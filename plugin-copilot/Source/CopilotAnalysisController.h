#pragma once

#include <JuceHeader.h>
#include <khensu/analysis/AnalysisResult.h>
#include <khensu/feedback/IssueState.h>
#include <khensu/recommendations/Recommendation.h>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace khensu {

/// Owns the plugin's non-real-time session refresh and its shared snapshot.
///
/// The worker never dereferences the processor or this controller: it holds
/// a shared_ptr to the State block, which outlives the controller. Nothing on
/// the message thread ever joins it (audit H3 - a join froze the DAW for a
/// whole 15-60 s analysis):
///   * loadLastSessionAsync() bumps a generation counter. ONE worker per
///     controller (PLG-11): when a load is already running the request is
///     queued, the running load sees its generation change at the next stage
///     boundary, stops without publishing, and the same worker then runs the
///     newest request. Superseded requests in between are dropped, so an
///     uncancellable stage (the native decode) never runs more than once at a
///     time however often the user clicks.
///   * the destructor raises a shutdown flag and only waits a short, bounded
///     time for the (cancelled) worker to leave plugin code. Every stage is
///     cancellable, including the deep-analysis HTTP wait (job + 100 ms polls
///     instead of one blocking request). If a worker still overruns the
///     budget, the plugin module is pinned so its code is never unmapped
///     under the running thread.
class CopilotAnalysisController final {
  public:
    /// Everything one load publishes, swapped in atomically under the lock.
    struct LoadedSnapshot {
        AnalysisResult analysis;
        std::vector<Recommendation> recs;
        /// Stored issue-queue state per finding (absent = open), read the
        /// way the desktop's queue reads it (issue_state, CON-1).
        std::map<std::string, issue_state::State> states;
        std::string sessionId;
        /// WF-8: graded from the desktop's stored measurements, not a
        /// fresh analysis of the file.
        bool fromStoredResult = false;
    };

    /// Returns true once the load should stop (superseded or shutting down).
    using CancelCheck = std::function<bool()>;
    /// The background pipeline. Must poll `cancelled` between stages and
    /// return std::nullopt when there is nothing to publish.
    using Loader = std::function<std::optional<LoadedSnapshot>(const CancelCheck& cancelled)>;

    /// `loader` is a test seam; empty uses loadLatestSession().
    explicit CopilotAnalysisController(Loader loader = {});
    ~CopilotAnalysisController();

    bool hasAnalysis() const noexcept;
    void getSnapshot(AnalysisResult& outAnalysis, std::vector<Recommendation>& outRecs) const;

    /// THREAD_CONTEXT(UIThread). Never blocks. onLoaded is posted to the
    /// message thread when this (latest) load finishes; a load superseded by
    /// a later call, or cut short by destruction, does not call back.
    void loadLastSessionAsync(std::function<void()> onLoaded);

    void setIssueState(const std::string& recId, issue_state::State state);
    /// Open when the store has no row for it.
    issue_state::State issueState(const std::string& recId) const;
    std::string getSessionId() const;

    /// Number of workers that have not yet left plugin code (tests).
    int getNumRunningWorkers() const;

    /// The production pipeline: the bound session (or the latest) -> native
    /// analysis -> optional cancellable deep analysis from the desktop-owned
    /// service -> recs against the session's genre (WF-8).
    static std::optional<LoadedSnapshot> loadLatestSession(const CancelCheck& cancelled);
    static std::optional<LoadedSnapshot> loadSession(const std::string& sessionId, const CancelCheck& cancelled);

    /// WF-8: the desktop session this instance follows ("" = the newest).
    /// The next load uses it. THREAD_CONTEXT(AnyThread)
    void setBoundSessionId(const std::string& sessionId);
    std::string boundSessionId() const;

    /// (id, name) of the desktop's recent sessions, newest first, for the
    /// session picker. Reads khensu.db read-only. THREAD_CONTEXT(UIThread)
    static std::vector<std::pair<std::string, std::string>> recentSessions(int limit = 12);

    /// Deep analysis of `path` through the desktop-owned service as a job
    /// polled every 100 ms (cancellable; the server job is cancelled too).
    /// True when the service result was merged into `analysis`. Also used by
    /// the ARA audio-source analysis (CE-DAW-1).
    static bool enrichFromService(AnalysisResult& analysis, const std::string& path, const CancelCheck& cancelled);

    /// Bounded wait the destructor allows a cancelled worker to finish.
    static constexpr int kShutdownWaitMs = 2500;

  private:
    struct State {
        juce::SpinLock mutex;
        std::optional<AnalysisResult> analysis;
        std::vector<Recommendation> recs;
        std::map<std::string, issue_state::State> states;
        std::string sessionId;

        std::atomic<uint64_t> generation{0};
        std::atomic<bool> shutdown{false};

        std::mutex runMutex;
        std::condition_variable runCv;
        int running = 0;
        /// Newest request queued behind the running worker (runMutex).
        bool pending = false;
        /// WF-8: the bound desktop session (mutex), "" = newest.
        std::string boundSessionId;
        std::function<void()> pendingOnLoaded;
    };

    static void runWorker(std::shared_ptr<State> state, uint64_t generation, Loader loader,
                          std::function<void()> onLoaded);

    std::shared_ptr<State> state_{std::make_shared<State>()};
    Loader loader_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CopilotAnalysisController)
};

} // namespace khensu
