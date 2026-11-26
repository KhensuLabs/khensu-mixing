// desktop-app/Source/RenderJobManager.h
#pragma once
#include <JuceHeader.h>
#include <khensu/ipc/HttpServiceBridge.h>
#include <atomic>
#include <functional>
#include <memory>
#include <map>
#include <optional>
#include <vector>

namespace khensu {

/// Result of evaluateGuardrailAlert() - see its doc comment.
struct GuardrailAlert {
    bool isBlock = false;
    juce::String details;
};

/// Pure decision logic for the post-render safety guardrail (2026-07-15
/// safety audit P0): given a render's fullResult JSON (the same shape
/// RenderJobManager::finishJob() receives), decide whether to surface a hard
/// block (a matched-level regression: true-peak/clipping/mono-fold) or a
/// softer warning (e.g. dynamics squashed, artifact-score delta), and what to
/// say. Returns std::nullopt when there is nothing to show (no "guardrail"
/// field, or guardOk with no warnings). Split out from finishJob() so this
/// decision logic is unit-testable without a live render/service. The
/// accept/reject path's remaining end-to-end coverage (cancel-after-jobId,
/// service-loss polling) landed in `RenderJobManagerTest.cpp`; see
/// docs/testing.md for what's still outstanding (desktop matched-level A/B
/// regression automation).
std::optional<GuardrailAlert> evaluateGuardrailAlert(const juce::var& fullResult);

/// Structured detail for a render whose job FAILED specifically because the
/// post-render guardrail was missing, errored, or rejected the output (audit
/// AI-01) - as opposed to failing for an infrastructure reason (service
/// unreachable, exception unrelated to the guardrail). The render was
/// quarantined rather than promoted to outputPath; an explicit expert
/// override (POST /copilot/override_guardrail) is the only way to promote it
/// anyway.
struct GuardrailRejection {
    juce::String guardrailStatus; // "missing" | "error" | "rejected"
    juce::String sourcePath;
    juce::String quarantinePath;
    juce::String outputPath;
    juce::var measurements; // present ("rejected") or void ("missing"/"error")
    juce::String sessionId;              // the render's session, for the override's provenance event
    juce::String sourceRecommendationId; // ditto: the recommendation that produced the chain
    juce::String decisionHash;           // ditto: the model decision that proposed it (empty if none)

    /// First-class terminal render state (audit AI-01): "rejected" when the
    /// guardrail ran and refused the output, "unvalidated" when it couldn't
    /// run at all ("missing"/"error", so safety is unknown). Prefers the
    /// server-supplied render_state when present, otherwise derives it from
    /// guardrailStatus so an older service still maps correctly.
    juce::String renderState() const
    {
        if (serverRenderState.isNotEmpty()) return serverRenderState;
        return guardrailStatus == "rejected" ? juce::String("rejected") : juce::String("unvalidated");
    }
    juce::String serverRenderState; // the service's own render_state field, if it sent one
};

/// Pure decision logic distinguishing a guardrail-caused job failure from a
/// generic one, given the failed job's result JSON (the shape router.py's
/// PostRenderGuardrailError branches populate: quarantine_path/
/// guardrail_status/source_path/output_path/measurements). Returns
/// std::nullopt for a generic failure (no quarantine_path present) - e.g. a
/// pre-render block or an unrelated exception, neither of which quarantined
/// anything to offer an override on.
std::optional<GuardrailRejection> describeGuardrailRejection(const juce::var& fullResult);

/// Message-thread coordinator for all audio renders that go through the
/// Python service's /copilot/apply_chain job queue. Every panel that used to
/// block a background thread on the synchronous applyChain()/applyGraph()
/// call now submits here instead, which buys three things at once:
///   1. progress ("rendering... 4s") and cancellation for every render,
///   2. one shared poll timer instead of N per-panel ones,
///   3. a single source of truth the global RenderQueueStrip can display.
///
/// Completion callbacks always run on the message thread. Broadcasts a
/// change message whenever the job list changes (job added/finished/status).
// THREAD_CONTEXT(UIThread)
class RenderJobManager final : public juce::ChangeBroadcaster, private juce::Timer {
  public:
    /// baseUrl defaults to the real local Python service; tests point this at
    /// a fake local HTTP server instead, so the cancel-after-jobId-assignment
    /// and service-loss polling-bound code paths can be exercised without a
    /// live service (see `RenderJobManagerTest.cpp`).
    explicit RenderJobManager(std::string baseUrl = ipc::HttpServiceBridge::kDefaultBaseUrl)
        : baseUrl_(std::move(baseUrl))
    {}
    ~RenderJobManager() override
    {
        stopTimer();
        for (auto& j : jobs_)
            j.stop->store(true); // ends any event stream at once (CancelWatch)
        pool_.removeAllJobs(true, 4000); // owned workers (were detached std::threads)
    }

    /// ok=true -> outputPathOrError is the rendered file path and fullResult
    /// holds the complete response (applied_effects, skipped_effects,
    /// harmonic_reports). ok=false -> outputPathOrError is an error message
    /// (empty when the job was cancelled by the user).
    using Callback = std::function<void(bool ok, const juce::String& outputPathOrError, const juce::var& fullResult)>;

    /// Central post-render safety guardrail alert (2026-07-15 safety audit P0):
    /// fires on job completion when the server flagged a matched-level
    /// regression. `isBlock` = a hard regression (true-peak/clipping/mono-fold)
    /// the user must be warned about before the render is treated as better;
    /// false = softer warnings (e.g. dynamics squashed). `details` is a
    /// newline-joined human-readable list. Set by AppController.
    std::function<void(bool isBlock, const juce::String& details)> onGuardrailAlert;

    /// Fires on job completion when the failure was specifically a guardrail
    /// rejection/error/missing-check (audit AI-01), so the caller can offer
    /// an explicit expert override instead of a generic "render failed"
    /// message. Set by AppController.
    std::function<void(const GuardrailRejection&)> onGuardrailRejected;

    using StemProvenanceProvider = std::function<std::map<juce::String, juce::String>()>;
    void setStemProvenanceProvider(StemProvenanceProvider provider) { stemProvenanceProvider_ = std::move(provider); }

    /// `targetStem` (audit AI-02): declares which stem this chain is meant to
    /// process ("mix" default, or "vocals"/"drums"/"bass"/"other") so the
    /// server's stem-attribution guardrail can catch a caller sending a
    /// vocals-targeted chain against `filePath` that doesn't actually
    /// resolve to a vocals stem. Empty defaults to "mix" server-side, same
    /// as every pre-existing call site that doesn't pass one.
    /// `decisionHash`: the decision_hash of the model response that proposed
    /// `chain` (Copilot chat), forwarded so the render's provenance event ties
    /// back to that decision. Leave empty for any render a model didn't
    /// propose - the key is then omitted from the request.
    void startChainRender(const juce::String& description, const juce::String& filePath, const juce::String& outputPath,
                          const std::vector<ipc::HttpServiceBridge::EffectSpec>& chain, const juce::String& sessionId,
                          const juce::String& sourceRecommendationId, Callback onDone, bool genreRelative = false,
                          bool genreActionAllowed = false, const juce::String& abstainedReason = {},
                          const juce::String& targetStem = {}, const juce::String& decisionHash = {},
                          const juce::String& keyStem = {});

    /// The chain from the most recent startChainRender() call, regardless of
    /// which panel/flow submitted it (chat apply, "Fix This", EQ apply, ...) -
    /// the single choke point every effect-chain render passes through.
    /// Backs the Preset Library's "Save Last Applied Chain" action. Empty
    /// before any chain render this run.
    const std::vector<ipc::HttpServiceBridge::EffectSpec>& lastChain() const noexcept { return lastChain_; }

    /// The decisionHash that came with lastChain() - empty when that chain
    /// wasn't model-proposed. Lets a re-use of lastChain() (batch render)
    /// keep the decision provenance.
    const juce::String& lastChainDecisionHash() const noexcept { return lastChainDecisionHash_; }

    /// OVH-9: what a render done in this process (no service) produced.
    /// `ok` -> outputPathOrError is the rendered file; else the error, empty
    /// when it stopped because it was cancelled.
    struct LocalRenderOutcome {
        bool ok = false;
        juce::String outputPathOrError;
        juce::var fullResult; ///< the service's RenderJobResult shape
    };
    /// Does the render on a worker; must poll `stop` and return soon after it
    /// turns true (temp files removed, nothing written).
    using LocalRenderWork = std::function<LocalRenderOutcome(const std::atomic<bool>& stop)>;

    /// OVH-9: runs `work` on this manager's own pool as one of its jobs, so a
    /// render done without the service is listed, cancellable (cancelJob /
    /// cancelAll set its stop flag) and drained on destruction like any other,
    /// and finishes through the same path - onRenderCompleted, the guardrail
    /// alert, then `onDone` on the message thread. A result that arrives after
    /// its job was cancelled is discarded and its output file deleted.
    void startLocalRender(const juce::String& description, const juce::String& inputPath,
                          const juce::String& sessionId, const juce::String& sourceRecommendationId,
                          LocalRenderWork work, Callback onDone);

    /// Records `chain` as lastChain() (every chain render, service or local).
    void rememberChain(const std::vector<ipc::HttpServiceBridge::EffectSpec>& chain, const juce::String& decisionHash)
    {
        lastChain_ = chain;
        lastChainDecisionHash_ = decisionHash;
    }

    void startGraphRender(const juce::String& description, const juce::String& filePath, const juce::String& outputPath,
                          const juce::var& branches, const juce::var& post, bool normalize,
                          const juce::String& sessionId, const juce::String& sourceRecommendationId, Callback onDone);

    /// Cooperative cancel: the job's callback fires once with ok=false and an
    /// empty error string, and the server discards the in-flight render result.
    /// Keyed by the stable localId (JobInfo::localId), NOT jobId - jobId is
    /// still empty while the /start call is in flight, and a job must stay
    /// cancellable through that window (frontend-review-2026-07-17.md P1).
    /// If jobId hasn't arrived yet, the job is removed from the visible list
    /// immediately and the late /start response is cancelled server-side as
    /// soon as its jobId arrives.
    void cancelJob(juce::uint32 localId);

    /// Cancels every queued/running render (each callback fires once with
    /// ok=false and an empty error, exactly like cancelJob()). Every render
    /// submitted here is session-scoped, so AppController calls this when the
    /// active session changes - a render of song A must never finish into
    /// song B's history, A/B side or export (AUDIT-002).
    void cancelAll();

    /// Fires (message thread) for every successfully completed render, before
    /// the job's own callback, with the render's input and output paths and
    /// the full result - lets AppController prime its per-path loudness cache
    /// from the guardrail's matched-gain measurement instead of re-measuring
    /// the file (AUDIT-038).
    std::function<void(const juce::String& inputPath, const juce::String& outputPath, const juce::var& fullResult)>
        onRenderCompleted;

    struct JobInfo {
        juce::uint32 localId = 0; // stable handle; always valid, use this to cancel
        juce::String jobId;       // empty while the /start call is still in flight
        juce::String description; // e.g. "Copilot chain -> mytrack_processed.wav"
        juce::String status;      // "starting" | "pending" | "running"
        juce::int64 startedMs = 0;
    };
    std::vector<JobInfo> activeJobs() const;

    /// Test seam: finishes a queued job exactly as the poll loop does when the
    /// service reports it done (callbacks, onRenderCompleted, guardrails).
    void finishJobForTesting(juce::uint32 localId, bool ok, const juce::String& outputPathOrError,
                             const juce::var& fullResult = {})
    {
        finishJob(localId, ok, outputPathOrError, fullResult);
    }

  private:
    struct Job {
        JobInfo info;
        Callback callback;
        juce::String inputPath; // the file the render reads (for onRenderCompleted)
        // Carried into a GuardrailRejection so an override is logged to the
        // same session's provenance timeline (the service skips it otherwise).
        juce::String sessionId, sourceRecommendationId, decisionHash;
        std::shared_ptr<std::atomic<bool>> pollInFlight = std::make_shared<std::atomic<bool>>(false);
        /// PERF-6: true while a worker follows the job's event stream (the
        /// poll timer skips it); `stop` ends that stream when the job ends.
        std::shared_ptr<std::atomic<bool>> streaming = std::make_shared<std::atomic<bool>>(false);
        std::shared_ptr<std::atomic<bool>> stop = std::make_shared<std::atomic<bool>>(false);
        juce::uint32 localId = 0; // stable handle before/independent of jobId
        bool local = false;       ///< OVH-9: rendered in-process (no jobId, never polled)
        // Bounds how long a job can sit "stuck" on a poll error before it's
        // surfaced as a service-loss failure instead of polling forever
        // (frontend-review-2026-07-17.md P1).
        int consecutivePollErrors = 0;
        juce::String lastPollError; ///< the last failed poll's error, for the message
        juce::int64 lastPollSuccessMs = 0;
    };

    void registerStart(Job&& job,
                       std::function<khensu::Result<juce::String, ipc::BridgeError>(ipc::HttpServiceBridge&)> startFn);
    void finishJob(juce::uint32 localId, bool ok, const juce::String& outputPathOrError, const juce::var& fullResult);
    void timerCallback() override;
    /// PERF-6: follow the job's status as server-sent events on a pool worker
    /// (at most kMaxStreams at once - they hold a worker for the whole
    /// render); the poll timer covers the others and takes over when a
    /// stream drops.
    void startFollowing(Job& job);
    /// One status (from a poll or the stream) applied on the message thread.
    void applyStatus(juce::uint32 localId, const ipc::HttpServiceBridge::RenderJobStatus& s);
    static constexpr int kMaxStreams = 2;
    std::shared_ptr<std::atomic<int>> activeStreams_ = std::make_shared<std::atomic<int>>(0);

    std::vector<Job> jobs_;
    std::vector<juce::uint32> pendingCancels_; // localIds cancelled while still "starting"
    juce::uint32 nextLocalId_ = 1;
    std::string baseUrl_;
    std::vector<ipc::HttpServiceBridge::EffectSpec> lastChain_;
    juce::String lastChainDecisionHash_;
    StemProvenanceProvider stemProvenanceProvider_;

    JUCE_DECLARE_WEAK_REFERENCEABLE(RenderJobManager)
    // Start/poll/cancel requests run here instead of detached std::threads,
    // so none can outlive the manager.
    juce::ThreadPool pool_{4};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RenderJobManager)
};

} // namespace khensu
