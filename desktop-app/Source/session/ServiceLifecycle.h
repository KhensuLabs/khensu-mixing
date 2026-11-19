// desktop-app/Source/session/ServiceLifecycle.h
#pragma once
#include <JuceHeader.h>
#include <optional>
#include <atomic>
#include <functional>
#include <memory>
#include <set>
#include <vector>
#include <khensu/ipc/HttpServiceBridge.h>
#include "../PythonServiceManager.h"
#include "../services/IServiceLifecycle.h"
#include "ServiceHealthMonitor.h"

namespace khensu {

class IBackgroundWork;
class SessionService;

/// The local Python service and the Copilot plugin link (ENG-1 stage C4,
/// extracted from AppController.cpp / AppController_Playback.cpp). Owns the
/// PythonServiceManager and the ServiceHealthMonitor, the service status the
/// sidebar shows, restart and diagnostics, the transactional outbox dispatch
/// (ARCH-03), the plugin parameter push / fetch and instance targeting
/// (PLG-04), the playhead FIFO, the reference-library fetch and the
/// cross-process event poll's dedup.
///
/// It owns no timer: AppController's 50 ms timer calls tick() with its tick
/// counter, so every poll keeps its cadence. Everything that is UI (sidebar
/// dot, error banner, the "gave up" prompt, the re-analysis, meters, the
/// reasoning window) goes through the hooks below, wired by the owner.
///
/// Message thread, except the background jobs it queues (on `work` and on the
/// poll / restart runners); their results come back on the message thread
/// through a WeakReference. The owner must drain those pools before
/// destroying this (AppController's BackgroundRunner does).
class ServiceLifecycle final : public IServiceLifecycle {
  public:
    /// The health monitor's /health timeout (see startHealthCheck).
    static constexpr int kHealthProbeTimeoutMs = 2500;

    /// Runs a job on the poll pool (BackgroundRunner::runPoll).
    using PollRunner = std::function<void(std::function<void()>)>;
    /// Runs a job on the one-thread restart pool; false (job dropped) while
    /// one is in flight (BackgroundRunner::runRestart).
    using RestartRunner = std::function<bool(std::function<void()>)>;
    using PlayheadState = ipc::HttpServiceBridge::PlayheadState;

    ServiceLifecycle(IBackgroundWork& work, SessionService& sessions, PollRunner runPoll, RestartRunner runRestart);
    ~ServiceLifecycle() override;

    // ---- Hooks (message thread), wired by the owner ----
    /// The service status changed (or was re-asserted) - the sidebar dot.
    std::function<void(PythonStatus status, int restartAttempt)> onStatusChanged;
    /// Every health check that found the service Running (after the status).
    std::function<void()> onServiceRunning;
    /// The unhealthy -> healthy edge: re-run a stale C++-only analysis.
    std::function<void()> onServiceRecovered;
    /// First "gave up restarting" of an outage; `portTaken` = another
    /// program owns the service port (DSK-17).
    std::function<void(bool portTaken)> onGaveUp;
    /// A connected plugin's latest playhead/meter frame.
    std::function<void(const PlayheadState&)> onPluginPlayhead;
    /// The newest new "show_reasoning" event of the active session.
    std::function<void(const juce::String& recommendationId)> onShowReasoningEvent;
    /// Shows a titled text report (the service log notice, diagnostics).
    std::function<void(const juce::String& title, const juce::String& category, const juce::String& text)> onShowReport;

    // ---- IServiceLifecycle ----
    PythonStatus getPythonServiceStatus() const noexcept override { return status_; }
    void restartPythonService() override;
    void copyServiceLogsToClipboard() override;
    void diagnoseServiceStatus() override;

    /// Drains SessionManager's transactional outbox (audit ARCH-03) if the
    /// service is reachable and there's anything pending - called from the
    /// periodic health-check tick whenever the service is Running, so a
    /// mirror that failed (or was enqueued while offline) gets retried
    /// without needing a dedicated timer of its own. No-op (cheap: one
    /// COUNT query) when the outbox is already empty. Message-thread only;
    /// the actual dispatch runs on the background pool.
    void dispatchOutboxIfNeeded() override;
    /// Count of not-yet-delivered outbox entries, for a diagnostics view
    /// (audit ARCH-03).
    int undeliveredOutboxCount() const noexcept override;

    /// Pushes updated built-in DSP parameters to the local FastAPI service.
    /// The VST3 plugin polls these and applies them in the DAW in real-time.
    void pushPluginParams(const std::map<juce::String, float>& params) override;
    /// Fetches the last pushed DSP parameters from the local FastAPI service.
    /// Calls onComplete(params) on the MessageManager thread upon success.
    void fetchPluginParams(std::function<void(const std::map<juce::String, float>&)> onComplete) override;
    void stagePluginChain(const std::map<juce::String, float>& params, const juce::String& source) override;
    std::optional<StagedPluginChain> stagedPluginChain() const override { return stagedChain_; }
    void applyStagedPluginChain() override;
    void discardStagedPluginChain() override;
    void addPluginChainListener(void* key, std::function<void()> cb) override { chainListeners_[key] = std::move(cb); }
    void removePluginChainListener(void* key) override { chainListeners_.erase(key); }
    /// Which Copilot plugin instance parameter pushes address (PLG-04,
    /// desktop half): "" = every instance (the default), else one instance
    /// id as the plugin reports it on /transport/playhead. Message thread.
    juce::String pluginTargetInstance() const override { return pluginTargetInstance_; }
    void setPluginTargetInstance(const juce::String& instanceId) override { pluginTargetInstance_ = instanceId; }
    /// The plugin instances the service last reported (playhead poll).
    const std::vector<juce::String>& knownPluginInstances() const noexcept override { return knownPluginInstances_; }

    /// Fetches the raw reference-library sample points for one genre (async
    /// - the underlying bridge call is blocking, so this runs it on the
    /// background pool and delivers the result back on the message thread).
    /// Empty vector on any error (service unreachable, unknown genre, etc.)
    /// - callers show their own empty state rather than distinguishing error
    /// kinds here.
    void getReferenceLibraryPoints(const juce::String& genreId,
                                   std::function<void(std::vector<ipc::GenreReferencePoint>)> onComplete) override;

    // ---- Owner API (AppController) ----
    /// The bearer token the /health identity challenge is keyed on (AUDIT-020).
    void setAuthToken(const juce::String& token) { pythonService_.setAuthToken(token); }
    /// Shows "starting" immediately (so the sidebar never flashes "stopped"
    /// while the background launch thread does its isAvailable() probe) and
    /// launches the service off the UI thread.
    void launch();
    /// Blocking graceful stop; called from ~AppController.
    void shutdown() { pythonService_.shutdown(); }
    /// The user asked for a manual restart: the next "gave up" may notify again.
    void resetGaveUpNotice() noexcept { serviceHealth_.resetGaveUpNotice(); }
    /// One step of the owner's 50 ms timer, `ticks` being its counter: the
    /// health check (every 32), the playhead poll (every 4), the event poll
    /// (every 16, with a session), then the latest playhead frame.
    void tick(int ticks);

    /// The health-decision step on its own (what a finished health check
    /// does on the message thread): status, outbox, recovery edge, restart
    /// with backoff and the gave-up notice. `processRunning` is sampled when
    /// the check started.
    void applyHealthResult(bool serviceResponding, bool processRunning, bool portListening = true);
    /// Takes the newest queued playhead frame, dropping older ones; false
    /// when none arrived since the last call.
    bool popLatestPlayheadState(PlayheadState& state);
    /// Queues one playhead frame (what the playhead poll does on its worker).
    void pushPlayheadState(const PlayheadState& state);
    /// Applies one event-poll result for `sessionId` (dedup + seeding).
    void processPolledEvents(const juce::String& sessionId,
                             const std::vector<ipc::HttpServiceBridge::ProvenanceEvent>& events);

  private:
    void setStatus(PythonStatus status, int restartAttempt = 0);
    void startHealthCheck();
    void startPlayheadPoll();
    void startEventPoll();

    IBackgroundWork& work_;
    SessionService& sessions_;
    PollRunner runPoll_;
    RestartRunner runRestart_;

    PythonServiceManager pythonService_;
    // Deep-analysis auto-recovery: if the session was analyzed while the Python
    // service was unreachable it settles to a C++-only result (no BPM/key/genre,
    // no drums/loudness deep data, "deep analysis pending" panels). When the
    // service later becomes healthy we re-run analysis once, on the
    // unhealthy->healthy EDGE, so it never leaves a stale C++-only result behind.
    ServiceHealthMonitor serviceHealth_; // health edge + one-shot gave-up rules
    PythonStatus status_ = PythonStatus::Stopped;

    // Outbox dispatch single-flight guard (shared so a pass can finish after teardown).
    std::shared_ptr<std::atomic<bool>> outboxInFlightFlag_ = std::make_shared<std::atomic<bool>>(false);
    std::atomic<bool>& outboxDispatchInFlight_ = *outboxInFlightFlag_;

    // In-flight guards prevent concurrent timer-spawned HTTP threads.
    // Without these, a new thread is launched every 200 ms regardless of whether
    // the previous request has returned, leading to unbounded thread accumulation
    // when the Python service is slow or unreachable.
    std::atomic<bool> playheadPollInFlight_{false};
    std::atomic<bool> healthCheckInFlight_{false};
    std::atomic<bool> eventPollInFlight_{false};

    juce::AbstractFifo playheadFifo_{16};
    PlayheadState playheadRingBuffer_[16];
    juce::String pluginTargetInstance_;              ///< "" = all instances
    std::optional<StagedPluginChain> stagedChain_;   ///< WF-9
    std::map<void*, std::function<void()>> chainListeners_;
    void notifyPluginChain();
    std::vector<juce::String> knownPluginInstances_; ///< from the latest playhead poll

    // Cross-process event poll (message thread only): which session's events
    // have been seeded, and every event id already handled for it.
    juce::String polledEventsSessionId_;
    std::set<juce::String> seenEventIds_;

    JUCE_DECLARE_WEAK_REFERENCEABLE(ServiceLifecycle)
    JUCE_DECLARE_NON_COPYABLE(ServiceLifecycle)
};

} // namespace khensu
