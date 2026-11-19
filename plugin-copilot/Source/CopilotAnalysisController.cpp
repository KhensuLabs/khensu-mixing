#include "CopilotAnalysisController.h"
#include "PluginWorkerPool.h"
#include "CopilotServiceClient.h"
#include <khensu/ipc/generated/ApiJobStatus.h>
#include "ModuleLifetime.h"

#include <khensu/analysis/AudioAnalyzer.h>
#include <khensu/common/AppPaths.h>
#include <khensu/feedback/FeedbackManager.h>
#include <khensu/ipc/HttpServiceBridge.h>
#include <khensu/recommendations/RecommendationEngine.h>
#include <khensu/session/SessionManager.h>
#include <khensu/targets/GenreEntry.h>
#include <khensu/targets/GenreDatabase.h>
#include "KhensuCopilotGenreData.h"

#include <chrono>
#include <thread>

namespace khensu {

/// Deep analysis through the desktop-owned service, as a job polled every
/// 100 ms so a cancel (superseded load / plugin unload) stops the wait almost
/// immediately and cancels the server-side job too. The old single blocking
/// /analyze/extended request could hold the worker for a whole analysis.
bool CopilotAnalysisController::enrichFromService(AnalysisResult& analysis, const std::string& path,
                                                  const CancelCheck& cancelled)
{
    // Short per-request timeouts: each blocking call bounds the cancel latency.
    auto bridge = copilot::ServiceClient::bridge(copilot::ServiceClient::kJobRequestMs);
    if (cancelled() || !bridge.is_service_available() || cancelled()) return false;

    auto started = bridge.startAnalysisJob(path, ipc::ILocalServiceBridge::AnalysisMode::Advanced);
    if (!started.is_ok()) return false;
    const std::string jobId = started.value();

    constexpr juce::uint32 kDeadlineMs = 180 * 1000;
    constexpr int kPollMs = 100;
    constexpr int kMaxConsecutivePollFailures = 20;
    const juce::uint32 startMs = juce::Time::getMillisecondCounter();
    int failures = 0;
    ipc::AnalysisJobStatus status;
    for (;;) {
        if (cancelled() || juce::Time::getMillisecondCounter() - startMs > kDeadlineMs) {
            bridge.cancelAnalysisJob(jobId);
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(kPollMs));
        auto poll = bridge.getAnalysisProgress(jobId);
        if (!poll.is_ok()) {
            if (++failures >= kMaxConsecutivePollFailures) return false;
            continue;
        }
        failures = 0;
        status = poll.value();
        if (khensu::api::isTerminalStatus(status.status))
            break;
    }
    if (!status.has_result || cancelled()) return false;

    // The finished job's result is the same payload /analyze/extended returns
    // and the service has it cached, so this re-fetch resolves immediately and
    // reuses the existing parser.
    if (auto deep = bridge.analyze(path, ipc::ILocalServiceBridge::AnalysisMode::Advanced); deep.is_ok()) {
        deep.value().apply_to(analysis.python_data);
        analysis.python_analysis_ran = true;
        analysis.mono_compat = deep.value().mono_compat;
        analysis.translation_risk = deep.value().translation_risk;
        analysis.harshness = deep.value().harshness;
        analysis.resonance = deep.value().resonance;
        analysis.mid_side_imbalance = deep.value().mid_side_imbalance;
        return true;
    }
    return false;
}

CopilotAnalysisController::CopilotAnalysisController(Loader loader) : loader_(std::move(loader))
{
    if (!loader_) {
        // The bound session is read when the load runs, through the State
        // block the worker already holds (never through `this`).
        std::weak_ptr<State> weak = state_;
        loader_ = [weak](const CancelCheck& cancelled) {
            std::string bound;
            if (auto state = weak.lock()) {
                juce::SpinLock::ScopedLockType lock(state->mutex);
                bound = state->boundSessionId;
            }
            return bound.empty() ? loadLatestSession(cancelled) : loadSession(bound, cancelled);
        };
    }
}

void CopilotAnalysisController::setBoundSessionId(const std::string& sessionId)
{
    juce::SpinLock::ScopedLockType lock(state_->mutex);
    state_->boundSessionId = sessionId;
}

std::string CopilotAnalysisController::boundSessionId() const
{
    juce::SpinLock::ScopedLockType lock(state_->mutex);
    return state_->boundSessionId;
}

std::vector<std::pair<std::string, std::string>> CopilotAnalysisController::recentSessions(int limit)
{
    std::vector<std::pair<std::string, std::string>> out;
    try {
        SessionManager mgr(AppPaths::databaseFile(), SessionManager::OpenMode::ReadOnly);
        if (auto sessions = mgr.listSessions(limit); sessions.is_ok())
            for (const auto& s : sessions.value())
                out.emplace_back(s.id, s.name);
    } catch (const std::exception&) {
        // No desktop session store yet: an empty picker.
    }
    return out;
}

namespace {
/// WF-8: the genre targets the plugin carries (binary data), loaded once.
const GenreDatabase& embeddedGenres()
{
    static const GenreDatabase db = [] {
        GenreDatabase g;
        g.loadFromString(std::string(KhensuCopilotData::genre_targets_json, KhensuCopilotData::genre_targets_jsonSize));
        return g;
    }();
    return db;
}
} // namespace

CopilotAnalysisController::~CopilotAnalysisController()
{
    // Never join: cancel, then give the worker a short, bounded window to
    // leave plugin code (every stage polls the flag, so this is normally a
    // few milliseconds). The State block it holds outlives this controller.
    state_->shutdown.store(true);
    std::unique_lock<std::mutex> lock(state_->runMutex);
    const bool finished = state_->runCv.wait_for(lock, std::chrono::milliseconds(kShutdownWaitMs),
                                                 [this] { return state_->running == 0; });
    if (!finished) {
        // A stage that cannot be interrupted (e.g. a long native decode) is
        // still running. Keep the module mapped for the process lifetime
        // rather than let the host unload code the thread is executing.
        pinPluginModuleForProcessLifetime();
    }
}

bool CopilotAnalysisController::hasAnalysis() const noexcept
{
    juce::SpinLock::ScopedLockType lock(state_->mutex);
    return state_->analysis.has_value();
}

void CopilotAnalysisController::getSnapshot(AnalysisResult& outAnalysis, std::vector<Recommendation>& outRecs) const
{
    juce::SpinLock::ScopedLockType lock(state_->mutex);
    if (state_->analysis) {
        outAnalysis = *state_->analysis;
        outRecs = state_->recs;
    }
}

int CopilotAnalysisController::getNumRunningWorkers() const
{
    std::lock_guard<std::mutex> lock(state_->runMutex);
    return state_->running;
}

void CopilotAnalysisController::loadLastSessionAsync(std::function<void()> onLoaded)
{
    // Supersede (never join) any load still in flight.
    const uint64_t generation = state_->generation.fetch_add(1) + 1;
    {
        std::lock_guard<std::mutex> lock(state_->runMutex);
        if (state_->running > 0) {
            // One worker per controller (PLG-11): queue behind it. The running
            // load notices the generation change and hands over.
            state_->pending = true;
            state_->pendingOnLoaded = std::move(onLoaded);
            return;
        }
        ++state_->running;
    }
    // ENG-9: the plugin's owned worker pool, not a detached thread per load.
    auto state = state_;
    auto loader = loader_;
    const bool queued = copilot::PluginWorkerPool::submit(
        [state, generation, loader, onLoaded = std::move(onLoaded)]() mutable {
            runWorker(state, generation, loader, std::move(onLoaded));
        });
    if (!queued) {
        std::lock_guard<std::mutex> lock(state_->runMutex);
        --state_->running;
        state_->runCv.notify_all();
        juce::Logger::writeToLog("Plugin background analysis refresh could not start a worker");
    }
}

void CopilotAnalysisController::runWorker(std::shared_ptr<State> state, uint64_t generation, Loader loader,
                                          std::function<void()> onLoaded)
{
    for (;;) {
        const CancelCheck cancelled = [&state, generation] {
            return state->shutdown.load() || state->generation.load() != generation;
        };

        std::optional<LoadedSnapshot> loaded;
        try {
            loaded = loader(cancelled);
        } catch (const std::exception& e) {
            if (!state->shutdown.load())
                juce::Logger::writeToLog(juce::String("Plugin background analysis refresh failed: ") + e.what());
        } catch (...) {
            if (!state->shutdown.load())
                juce::Logger::writeToLog("Plugin background analysis refresh failed: unknown error");
        }

        bool deliver = false;
        {
            juce::SpinLock::ScopedLockType lock(state->mutex);
            // Checked under the lock so a newer load (or shutdown) can never be
            // overwritten by this one's late result.
            if (!cancelled()) {
                deliver = true;
                if (loaded) {
                    state->analysis = std::move(loaded->analysis);
                    state->recs = std::move(loaded->recs);
                    state->states = std::move(loaded->states);
                    state->sessionId = std::move(loaded->sessionId);
                }
            }
        }
        loaded.reset();
        if (deliver && onLoaded) juce::MessageManager::callAsync(std::move(onLoaded));
        onLoaded = nullptr;

        // Hand over to the newest queued request, or retire. Last touch of
        // shared state when retiring: after this the destructor may return.
        std::lock_guard<std::mutex> lock(state->runMutex);
        if (state->pending && !state->shutdown.load()) {
            state->pending = false;
            onLoaded = std::move(state->pendingOnLoaded);
            state->pendingOnLoaded = nullptr;
            generation = state->generation.load();
            continue;
        }
        state->pending = false;
        state->pendingOnLoaded = nullptr;
        loader = nullptr;
        --state->running;
        state->runCv.notify_all();
        return;
    }
}

std::optional<CopilotAnalysisController::LoadedSnapshot>
CopilotAnalysisController::loadLatestSession(const CancelCheck& cancelled)
{
    // Read-only: a plugin instance never creates or migrates khensu.db - every
    // instance used to race the desktop app's migration runner (AUDIT-013).
    std::unique_ptr<SessionManager> mgrHolder;
    try {
        mgrHolder = std::make_unique<SessionManager>(AppPaths::databaseFile(), SessionManager::OpenMode::ReadOnly);
    } catch (const std::exception&) {
        return std::nullopt; // no desktop session store yet (or it is unreadable)
    }
    auto& mgr = *mgrHolder;
    auto sessions = mgr.listSessions(1);
    if (!sessions.is_ok() || sessions.value().empty() || sessions.value()[0].files.empty()) return std::nullopt;
    if (cancelled()) return std::nullopt;
    return loadSession(sessions.value()[0].id, cancelled);
}

std::optional<CopilotAnalysisController::LoadedSnapshot>
CopilotAnalysisController::loadSession(const std::string& sessionId, const CancelCheck& cancelled)
{
    std::unique_ptr<SessionManager> mgrHolder;
    try {
        mgrHolder = std::make_unique<SessionManager>(AppPaths::databaseFile(), SessionManager::OpenMode::ReadOnly);
    } catch (const std::exception&) {
        return std::nullopt;
    }
    auto loaded = mgrHolder->loadSession(sessionId);
    if (!loaded.is_ok() || !loaded.value().has_value() || loaded.value()->files.empty()) return std::nullopt;
    if (cancelled()) return std::nullopt;
    const Session session = *loaded.value();
    LoadedSnapshot snapshot;
    // WF-8: grade what the desktop measured when it stored the measurements
    // (no decode, no second analysis, the same numbers as the desktop);
    // analyse the file only for a session analysed before they were stored.
    auto stored = mgrHolder->loadLatestMeasured(session.id, session.files[0].id);
    if (stored.is_ok() && stored.value().has_value()) {
        snapshot.analysis = std::move(*stored.value());
        snapshot.fromStoredResult = true;
    } else {
        AudioAnalyzer analyzer;
        auto result = analyzer.analyze(session.files[0], session.id);
        if (!result.is_ok() || cancelled()) return std::nullopt;
        snapshot.analysis = std::move(result.value());
    }
    if (cancelled()) return std::nullopt;

    // Enrich the local result when the desktop-owned service is available.
    // The plugin remains useful without it.
    enrichFromService(snapshot.analysis, session.files[0].path, cancelled);
    if (cancelled()) return std::nullopt;

    RecommendationEngine engine;
    // WF-8: the session's own genre, as the desktop grades it (the default
    // entry made the plugin's findings differ from the desktop's).
    GenreEntry genre;
    if (auto g = embeddedGenres().findById(session.target.primary_genre)) genre = *g;
    snapshot.recs = engine.generate(snapshot.analysis, genre, session.target);

    try {
        // Read-only, never provisions khensu.db (PLG-10 / CORE-16).
        FeedbackManager feedback(AppPaths::databaseFile(), FeedbackManager::OpenMode::ReadOnlyNoProvision);
        // One query, with each row's note, so a finding dismissed on the
        // desktop reads dismissed here rather than open (CON-1).
        std::vector<std::string> ids;
        ids.reserve(snapshot.recs.size());
        for (const auto& recommendation : snapshot.recs)
            ids.push_back(recommendation.id);
        if (cancelled()) return std::nullopt;
        for (const auto& [id, row] : feedback.getFeedbackBatch(ids)) {
            const auto state = issue_state::of(row);
            if (state != issue_state::State::Open) snapshot.states[id] = state;
        }
    } catch (const std::exception& e) {
        juce::Logger::writeToLog(juce::String("Plugin issue-state load failed: ") + e.what());
    } catch (...) {
        juce::Logger::writeToLog("Plugin issue-state load failed: unknown error");
    }

    snapshot.sessionId = session.id;
    return snapshot;
}

void CopilotAnalysisController::setIssueState(const std::string& recId, issue_state::State state)
{
    juce::SpinLock::ScopedLockType lock(state_->mutex);
    if (state == issue_state::State::Open)
        state_->states.erase(recId);
    else
        state_->states[recId] = state;
}

issue_state::State CopilotAnalysisController::issueState(const std::string& recId) const
{
    juce::SpinLock::ScopedLockType lock(state_->mutex);
    const auto it = state_->states.find(recId);
    return it == state_->states.end() ? issue_state::State::Open : it->second;
}

std::string CopilotAnalysisController::getSessionId() const
{
    juce::SpinLock::ScopedLockType lock(state_->mutex);
    return state_->sessionId;
}

} // namespace khensu
