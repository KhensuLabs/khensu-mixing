// desktop-app/Source/session/ServiceLifecycle.cpp
// Moved from AppController.cpp / AppController_Playback.cpp (ENG-1 stage C4);
// behaviour unchanged.
#include "ServiceLifecycle.h"
#include "SessionService.h"
#include "../IBackgroundWork.h"
#include <khensu/ipc/generated/ApiSchema.h>
#include <khensu/session/OutboxDispatcher.h>
#include <khensu/session/SessionManager.h>
#include <khensu/common/AppPaths.h>
#include "../settings/OnlineSettings.h"

namespace khensu {

namespace apif = khensu::api::fields;

ServiceLifecycle::ServiceLifecycle(IBackgroundWork& work, SessionService& sessions, PollRunner runPoll,
                                   RestartRunner runRestart)
    : work_(work), sessions_(sessions), runPoll_(std::move(runPoll)), runRestart_(std::move(runRestart))
{}

ServiceLifecycle::~ServiceLifecycle() = default;

void ServiceLifecycle::setStatus(PythonStatus status, int restartAttempt)
{
    status_ = status;
    if (onStatusChanged) onStatusChanged(status, restartAttempt);
}

void ServiceLifecycle::launch()
{
    setStatus(PythonStatus::Starting);
    // Fire-and-forget: token is already on disk, launch runs off the UI thread.
    pythonService_.launchAsync();
}

void ServiceLifecycle::restartPythonService()
{
    // shutdown() blocks for seconds (graceful /shutdown, then kill); it
    // used to freeze the UI here (AUDIT-040). Its own one-thread pool,
    // not the long pool: a restart requested exactly when the service is
    // broken must not queue behind six stuck polls (DSK-10). A second
    // request while one is running is dropped.
    runRestart_([service = &pythonService_] { service->restartBlocking(); });
}

// ---- Timer-driven polls ------------------------------------------------------------

void ServiceLifecycle::tick(int ticks)
{
    // ---- Service health + Python status (every 32 ticks ~ 1.6s) ----
    // compare_exchange_strong prevents a new thread from launching while the
    // previous health-check request is still in flight.
    if (ticks % 32 == 0) startHealthCheck();

    // ---- Transport playhead sync from DAW plugin (every 4 ticks ~ 200ms) ----
    // Polled on a background thread and pushed to SPSC lock-free FIFO to prevent UI lag.
    if (ticks % 4 == 0) startPlayheadPoll();

    // ---- Cross-process events polling from Python service (every 16 ticks ~ 800ms) ----
    if (ticks % 16 == 0 && sessions_.state().session) startEventPoll();

    // Process latest playhead state from lock-free queue (draining stale frames)
    PlayheadState s;
    // Without a connected plugin the service returns placeholder meters
    // (correlation 0, width 1, -96 LUFS) that would overwrite analysed values.
    const bool gotPlayhead = popLatestPlayheadState(s);
    if (gotPlayhead) knownPluginInstances_ = s.instances; // the target chooser's list (PLG-04)
    if (gotPlayhead && s.connected && onPluginPlayhead) onPluginPlayhead(s);
}

void ServiceLifecycle::startHealthCheck()
{
    bool expected = false;
    if (!healthCheckInFlight_.compare_exchange_strong(expected, true)) return;
    const bool processRunning = pythonService_.isProcessRunning();
    juce::WeakReference<ServiceLifecycle> self(this);
    // pythonService_ outlives the poll pool (drained first in the owner's
    // destructor), so the raw pointer is safe on the worker.
    PythonServiceManager* service = &pythonService_;
    runPoll_([self, processRunning = processRunning, service]() mutable {
        // Version AND HMAC proof, not "any 200" (AUDIT-040/020); also
        // re-opens/closes the bridge's identity gate. 2.5 s, not 1 s: a
        // service busy with an intake (scans, Demucs) can take over a second
        // to answer, and three such misses closed the identity gate and
        // showed "Stopped" while it was healthy. The in-flight flag keeps
        // ticks from stacking while a probe waits.
        const bool running = service->verifyServiceIdentity(kHealthProbeTimeoutMs);
        // Only Windows can read the listener table; elsewhere "listening" is
        // assumed, so a wedged service is never restarted on a guess.
#if JUCE_WINDOWS
        const bool portListening =
            running || !PythonServiceManager::listenerPids(PythonServiceManager::kPort).empty();
#else
        const bool portListening = true;
#endif
        // An adopted service has no child handle, so one slow probe (a busy
        // engine) read as "process gone" and showed "not running" while it
        // worked (RUN-14). Alive = still listening on the port.
        if (!processRunning && service->adoptedService() && portListening) processRunning = true;
        juce::MessageManager::callAsync([self, running, processRunning, portListening]() {
            if (self == nullptr) return;
            self->healthCheckInFlight_.store(false);
            self->applyHealthResult(running, processRunning, portListening);
        });
    });
}

void ServiceLifecycle::applyHealthResult(bool running, bool processRunning, bool portListening)
{
    using HS = ServiceHealthMonitor::Status;
    auto& svc = pythonService_;
    // DSK-22: canAttemptRestart() schedules the next backoff
    // step as a side effect - ask it ONLY when the service is
    // actually stopped. Called every tick, a long first-run
    // pip install (process alive, not yet healthy) pushed the
    // backoff to its 30 s cap before the first real failure.
    const bool gaveUp = svc.hasGivenUp();
    const bool stopped = !running && !processRunning && !gaveUp;
    const bool mayRestart = stopped && svc.canAttemptRestart();
    const auto d = serviceHealth_.onHealthResult(running, processRunning, gaveUp, mayRestart, portListening);
    if (static_cast<int>(d.status) != lastLoggedHealth_) { // RUN-14: why the status moved
        lastLoggedHealth_ = static_cast<int>(d.status);
        juce::Logger::writeToLog("ServiceLifecycle: health " + juce::String(lastLoggedHealth_) + " responding="
                                 + juce::String((int)running) + " process=" + juce::String((int)processRunning)
                                 + " listening=" + juce::String((int)portListening) + " adopted="
                                 + juce::String((int)svc.adoptedService()) + " restart="
                                 + juce::String((int)d.attemptRestart) + " wedged=" + juce::String((int)d.restartWedged));
    }
    switch (d.status) {
    case HS::Running:
        setStatus(PythonStatus::Running);
        svc.notifyHealthy();
        pushCredentialsIfNeeded();
        if (onServiceRunning) onServiceRunning(); // the once-per-launch update check
        break;
    case HS::Starting:
        // Normal cold start: 0 = "starting...", not "restarting (attempt N)".
        setStatus(PythonStatus::Starting, 0);
        break;
    case HS::GaveUp:
        // Distinct from Stopped; persists after the one-time modal.
        setStatus(PythonStatus::GaveUp);
        break;
    case HS::Stopped:
        credentialsSentMs_->store(0); // a restarted service starts empty
        setStatus(PythonStatus::Stopped, svc.consecutiveFailureCount());
        break;
    }
    // Retry outbox entries left over from an offline period (ARCH-03).
    if (d.dispatchOutbox) dispatchOutboxIfNeeded();
    // Healthy edge: re-run a stale C++-only analysis once.
    if (d.reanalyze && onServiceRecovered) onServiceRecovered();
    // Auto-restart with backoff (a crash-on-launch service would
    // otherwise respawn every ~1.6 s tick forever).
    if (d.attemptRestart) svc.launchAsync();
    // Alive but no longer listening (a shutdown that never finished): the
    // launch above never fires for a live process, so kill and relaunch it.
    if (d.restartWedged) {
        juce::Logger::writeToLog("ServiceLifecycle: service process alive but not listening - restarting it");
        restartPythonService();
    }
    // DSK-17: the owner names the real cause when another program owns the
    // service port - "check Python" sent users after the wrong problem.
    if (d.notifyGaveUp && onGaveUp) onGaveUp(svc.portInUseByOtherProcess());
}

void ServiceLifecycle::pushCredentialsIfNeeded()
{
    const auto configDir = juce::File(juce::String(AppPaths::dataDirectory()));
    const auto settingsFile = configDir.getChildFile("settings.json");
    const juce::int64 stamp =
        settingsFile.existsAsFile() ? settingsFile.getLastModificationTime().toMilliseconds() : 1;
    if (stamp == credentialsSentMs_->load() || credentialsInFlight_->exchange(true)) return;
    auto inFlight = credentialsInFlight_;
    auto sentMs = credentialsSentMs_;
    work_.runInBackground([configDir, stamp, inFlight, sentMs] {
        const auto s = OnlineSettings::load(configDir);
        const std::map<std::string, std::string> keys{
            {"acoustid.api_key", s.acoustidApiKey.toStdString()},
            {"llm.anthropic_api_key", s.anthropicApiKey.toStdString()},
            {"llm.gemini_api_key", s.geminiApiKey.toStdString()},
            {"llm.openai_api_key", s.openaiApiKey.toStdString()},
            {"llm.deepseek_api_key", s.deepseekApiKey.toStdString()},
            {"llm.moonshot_api_key", s.moonshotApiKey.toStdString()},
        };
        if (ipc::HttpServiceBridge{}.sendSessionCredentials(keys).is_ok()) sentMs->store(stamp);
        inFlight->store(false);
    });
}

void ServiceLifecycle::startPlayheadPoll()
{
    bool expected = false;
    if (!playheadPollInFlight_.compare_exchange_strong(expected, true)) return;
    juce::WeakReference<ServiceLifecycle> self(this);
    runPoll_([self]() {
        ipc::HttpServiceBridge bridge(PythonServiceManager::kBaseUrl, 1000, 0);
        auto result = bridge.getPlayhead();
        if (self != nullptr) {
            if (result.is_ok()) self->pushPlayheadState(result.value());
            self->playheadPollInFlight_.store(false);
        }
    });
}

void ServiceLifecycle::pushPlayheadState(const PlayheadState& s)
{
    int start1, size1, start2, size2;
    playheadFifo_.prepareToWrite(1, start1, size1, start2, size2);
    if (size1 > 0) {
        playheadRingBuffer_[start1] = s;
        playheadFifo_.finishedWrite(1);
    }
}

bool ServiceLifecycle::popLatestPlayheadState(PlayheadState& state)
{
    int numReady = playheadFifo_.getNumReady();
    if (numReady == 0) return false;

    int start1, size1, start2, size2;
    playheadFifo_.prepareToRead(numReady, start1, size1, start2, size2);

    if (size1 > 0) {
        if (size2 > 0) {
            state = playheadRingBuffer_[start2 + size2 - 1];
        } else {
            state = playheadRingBuffer_[start1 + size1 - 1];
        }
        playheadFifo_.finishedRead(numReady);
        return true;
    }
    return false;
}

void ServiceLifecycle::startEventPoll()
{
    bool expected = false;
    if (!eventPollInFlight_.compare_exchange_strong(expected, true)) return;
    juce::WeakReference<ServiceLifecycle> self(this);
    const juce::String sessionId = sessions_.state().session->id;
    runPoll_([self, sessionId]() {
        ipc::HttpServiceBridge bridge;
        auto result = bridge.getEvents(sessionId, 5);
        std::vector<ipc::HttpServiceBridge::ProvenanceEvent> events;
        if (result.is_ok()) events = result.value();
        // All state is touched on the message thread (this worker used
        // to write lastProcessedEventId_ and read SafePointer here).
        juce::MessageManager::callAsync([self, sessionId, events = std::move(events)]() {
            if (self == nullptr) return;
            self->eventPollInFlight_.store(false);
            self->processPolledEvents(sessionId, events);
        });
    });
}

void ServiceLifecycle::processPolledEvents(const juce::String& sessionId,
                                           const std::vector<ipc::HttpServiceBridge::ProvenanceEvent>& events)
{
    if (!sessions_.state().session || juce::String(sessions_.state().session->id) != sessionId) return;

    // The first poll of a session only records what already exists: events
    // from before the app opened (or before a session switch) must not pop
    // the window. Afterwards each event id fires at most once - the previous
    // single "last id" let two alternating events re-fire on every poll and
    // steal focus.
    const bool seeding = polledEventsSessionId_ != sessionId;
    if (seeding) {
        polledEventsSessionId_ = sessionId;
        seenEventIds_.clear();
    }
    const ipc::HttpServiceBridge::ProvenanceEvent* newest = nullptr;
    for (const auto& ev : events) {
        if (seenEventIds_.count(ev.id) != 0) continue;
        seenEventIds_.insert(ev.id);
        if (!seeding && ev.event_type == "show_reasoning" && (newest == nullptr || ev.created_at > newest->created_at))
            newest = &ev;
    }
    if (seenEventIds_.size() > 512) seenEventIds_.clear(); // bounded; the service returns the latest 5
    for (const auto& ev : events)
        seenEventIds_.insert(ev.id);
    if (newest != nullptr && onShowReasoningEvent) onShowReasoningEvent(newest->recommendation_id);
}

// ---- Diagnostics -------------------------------------------------------------------

void ServiceLifecycle::copyServiceLogsToClipboard()
{
    const auto lines = pythonService_.getRecentLogLines();
    juce::SystemClipboard::copyTextToClipboard(
        lines.isEmpty() ? juce::String("(service log empty - no output captured yet)") : lines.joinIntoString("\n"));
    if (onShowReport)
        onShowReport(TRANS("Service log"), TRANS("Diagnose"),
                     lines.isEmpty()
                         ? TRANS("No service output captured yet.")
                         : juce::String(lines.size()) + " " + TRANS("service log lines copied to the clipboard."));
}

void ServiceLifecycle::diagnoseServiceStatus()
{
    const bool procRunning = pythonService_.isProcessRunning();
    const bool stale = pythonService_.staleServiceDetected();
    const int failures = pythonService_.consecutiveFailureCount();
    const bool portTaken = pythonService_.portInUseByOtherProcess();
    const juce::String lastFailure = pythonService_.lastLaunchFailureReason();
    const int undeliveredOutbox = undeliveredOutboxCount();
    auto logLines = pythonService_.getRecentLogLines();
    juce::WeakReference<ServiceLifecycle> self(this);
    work_.runInBackground([self, procRunning, stale, failures, portTaken, lastFailure, undeliveredOutbox, logLines]() {
        ipc::HttpServiceBridge bridge;
        auto meta = bridge.getServiceMetadata();
        juce::String report;
        report << "PROCESS\n";
        report << "  child running: " << (procRunning ? "yes" : "no") << "\n";
        report << "  stale build on port: " << (stale ? "yes" : "no") << "\n";
        report << "  consecutive launch failures: " << juce::String(failures) << "\n";
        report << "  port " << juce::String(PythonServiceManager::kPort)
               << " owned by another process: " << (portTaken ? "yes" : "no") << "\n";
        if (lastFailure.isNotEmpty()) report << "  last launch failure: " << lastFailure << "\n";
        report << "  undelivered feedback/provenance mirror events (outbox): " << juce::String(undeliveredOutbox)
               << "\n\n";
        report << "HEALTH (GET /health)\n";
        if (meta.is_ok()) {
            if (auto* o = meta.value().getDynamicObject()) {
                report << "  reachable: yes\n";
                report << "  version: " << o->getProperty(apif::AnalysisMetadataResponse::service_version).toString()
                       << "  (desktop expects " << juce::String(PythonServiceManager::kExpectedServiceVersion) << ")\n";
                report << "  python: " << o->getProperty(apif::AnalysisMetadataResponse::python_version).toString()
                       << "\n";
            } else {
                report << "  reachable: yes (no metadata)\n";
            }
        } else {
            report << "  reachable: no - " << meta.error().message << "\n";
        }
        report << "\nRECENT LOG (last " << juce::String(logLines.size()) << " lines)\n";
        const int tail = juce::jmin(40, logLines.size());
        for (int i = logLines.size() - tail; i < logLines.size(); ++i)
            report << "  " << logLines[i] << "\n";
        if (logLines.isEmpty()) report << "  (no service output captured this session)\n";

        juce::MessageManager::callAsync([self, report]() {
            if (self == nullptr) return;
            if (self->onShowReport)
                self->onShowReport(TRANS("Analysis service diagnostics"), TRANS("Diagnose"), report);
        });
    });
}

// ---- Transactional outbox (ARCH-03) ------------------------------------------------

int ServiceLifecycle::undeliveredOutboxCount() const noexcept
{
    const auto store = sessions_.sharedManager();
    return store ? store->undeliveredOutboxCount() : 0;
}

void ServiceLifecycle::dispatchOutboxIfNeeded()
{
    if (!sessions_.sharedManager() || sessions_.manager().undeliveredOutboxCount() == 0) return;

    // Captured by shared_ptr (mirrors autosaveState()'s pattern) so the
    // background dispatch can finish safely even if AppController is torn
    // down mid-flight.
    // One dispatcher at a time: this runs on every healthy tick (~1.6 s), and
    // two overlapping passes sent the same pending entries twice.
    bool expected = false;
    if (!outboxDispatchInFlight_.compare_exchange_strong(expected, true)) return;
    auto sessionMgrForThread = sessions_.sharedManager();
    auto inFlight = outboxInFlightFlag_;
    work_.runInBackground([sessionMgrForThread, inFlight]() {
        const struct Release {
            std::shared_ptr<std::atomic<bool>> flag;
            ~Release() { flag->store(false); }
        } release{inFlight};
        // Only entries whose retry backoff has elapsed; a permanent 4xx
        // (anything but 401/408/429) is dead-lettered instead of being
        // retried forever (AUDIT-056).
        using Outcome = OutboxDispatcher::SendOutcome;
        OutboxDispatcher::dispatchDue(
            *sessionMgrForThread, [](const std::string& kind, const std::string& payloadJson) -> Outcome {
                ipc::HttpServiceBridge bridge;
                const juce::var parsed = juce::JSON::parse(juce::String(payloadJson));
                if (!parsed.isObject()) return Outcome::Rejected; // unparseable payload: can never succeed
                int status = 0;
                if (kind == "feedback") {
                    bridge.postFeedback(parsed.getProperty("rec_id", juce::var()).toString(),
                                        static_cast<bool>(parsed.getProperty("acted_on", juce::var(false))),
                                        parsed.getProperty("note", juce::var()).toString(), &status);
                    return OutboxDispatcher::outcomeForHttpStatus(status);
                }
                if (kind == "event") {
                    bridge.postEvent(parsed.getProperty("session_id", juce::var()).toString(),
                                     parsed.getProperty("event_type", juce::var()).toString(),
                                     parsed.getProperty("summary", juce::var()).toString(),
                                     parsed.getProperty("rec_id", juce::var()).toString(),
                                     parsed.getProperty("payload", juce::var()), &status);
                    return OutboxDispatcher::outcomeForHttpStatus(status);
                }
                return Outcome::Rejected; // unknown kind - nothing could ever deliver it
            });
    });
}

// ---- Copilot plugin link -----------------------------------------------------------

void ServiceLifecycle::pushPluginParams(const std::map<juce::String, float>& params)
{
    // Addressed to the chosen instance ("" = all) - PLG-04: several Copilots
    // (vocal / bus / master) used to all apply every push.
    work_.runInBackground([params, target = pluginTargetInstance_]() {
        ipc::HttpServiceBridge bridge;
        bridge.postPluginParams(params, target);
    });
}

void ServiceLifecycle::stagePluginChain(const std::map<juce::String, float>& params, const juce::String& source)
{
    stagedChain_ = StagedPluginChain{params, source};
    notifyPluginChain();
}

void ServiceLifecycle::applyStagedPluginChain()
{
    if (!stagedChain_) return;
    pushPluginParams(stagedChain_->params);
    stagedChain_.reset();
    notifyPluginChain();
}

void ServiceLifecycle::discardStagedPluginChain()
{
    if (!stagedChain_) return;
    stagedChain_.reset();
    notifyPluginChain();
}

void ServiceLifecycle::notifyPluginChain()
{
    const auto listeners = chainListeners_; // a listener may deregister itself
    for (const auto& [key, cb] : listeners)
        if (chainListeners_.count(key) != 0 && cb) cb();
}

void ServiceLifecycle::fetchPluginParams(std::function<void(const std::map<juce::String, float>&)> onComplete)
{
    if (onComplete == nullptr) return;
    juce::WeakReference<ServiceLifecycle> self(this);
    work_.runInBackground([self, onComplete, target = pluginTargetInstance_]() {
        ipc::HttpServiceBridge bridge;
        auto result = bridge.pollPluginParams(target); // what the targeted instance would apply
        juce::MessageManager::callAsync([self, result = std::move(result), onComplete]() {
            if (self == nullptr) return;
            if (result.is_ok())
                onComplete(result.value().params);
            else
                onComplete({});
        });
    });
}

void ServiceLifecycle::getReferenceLibraryPoints(const juce::String& genreId,
                                                 std::function<void(std::vector<ipc::GenreReferencePoint>)> onComplete)
{
    if (onComplete == nullptr) return;
    juce::WeakReference<ServiceLifecycle> self(this);
    work_.runInBackground([self, genreId, onComplete]() {
        ipc::HttpServiceBridge bridge;
        auto result = bridge.getReferenceLibraryPoints(genreId.toStdString());
        juce::MessageManager::callAsync([self, result = std::move(result), onComplete]() {
            if (self == nullptr) return;
            if (result.is_ok())
                onComplete(result.value());
            else
                onComplete({});
        });
    });
}

} // namespace khensu
