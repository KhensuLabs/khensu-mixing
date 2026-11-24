// desktop-app/Source/analysis/ServiceReadyWait.h
// Whether the analysis engine is there for the deep pass (2026-11-09).
//
// AnalysisController used to ask once, with a single 1 s /health probe, and
// skip the whole deep pass on a miss - silently, as "fast analysis only - the
// analysis engine was unreachable". A cold start's first analysis races the
// intake's own scans (and Demucs) for the service, so one slow answer cost
// the user the deep results while the service was healthy. The fast result
// is already on screen by then, so waiting a few seconds blocks nothing.
// THREAD_CONTEXT(WorkerThread)
//
// OVH-1 (2026-11-23): that probe was a plain /health with a 1 s timeout,
// while ServiceLifecycle - what the sidebar shows - proves /health with HMAC
// and waits 2.5 s. A service busy with the intake's scans missed the short
// probe and the deep pass was skipped while the sidebar said "running", and
// no recovery re-run followed (the lifecycle never saw an outage). The deep
// pass now asks the lifecycle (EngineGate) and probes only when the
// lifecycle has not judged the engine yet (tests, the headless tools).
#pragma once
#include <atomic>
#include <functional>

namespace khensu::service_ready {

/// The lifecycle's verdict on the engine, published on the message thread
/// (ServiceLifecycle's status) and read on the analysis worker.
class EngineGate {
  public:
    enum class State { Unknown, Running, Starting, Down };
    void publish(State s) noexcept { state_.store(static_cast<int>(s), std::memory_order_release); }
    State state() const noexcept { return static_cast<State>(state_.load(std::memory_order_acquire)); }

  private:
    std::atomic<int> state_{static_cast<int>(State::Unknown)};
};

/// One availability answer for the deep pass: the lifecycle's when it has one
/// (Running = available, even while the engine is too busy to answer a quick
/// probe; Starting / Down = not yet), else `probe()`.
inline bool engineAvailable(const EngineGate* gate, const std::function<bool()>& probe)
{
    if (gate != nullptr) {
        switch (gate->state()) {
        case EngineGate::State::Running: return true;
        case EngineGate::State::Starting:
        case EngineGate::State::Down: return false;
        case EngineGate::State::Unknown: break;
        }
    }
    return probe ? probe() : false;
}

/// FN2-2b: the lifecycle has judged the engine and it is not running yet
/// (Starting, e.g. the first launch's cold start) or not at all (Down). The
/// deep pass then does not wait for it: the native result is published at
/// once as a complete result with the deep inputs marked unanswered, and the
/// analysis is re-run in place when the engine is Running
/// (AnalysisService::setEngineStatus). Unknown (no lifecycle: tests, the
/// headless tools) is not a verdict - those still probe and wait.
inline bool engineKnownNotReady(const EngineGate* gate) noexcept
{
    if (gate == nullptr) return false;
    const auto s = gate->state();
    return s == EngineGate::State::Starting || s == EngineGate::State::Down;
}

struct WaitResult {
    bool available = false;
    int probes = 0;
    int waitedMs = 0;
    bool cancelled = false;
};

/// Probes until `probe()` succeeds, `cancelled()` turns true or `windowMs`
/// has passed, sleeping `retryMs` between probes. `elapsedMs()` and
/// `sleepMs()` are injected so tests run without a clock.
inline WaitResult waitForService(const std::function<bool()>& probe, const std::function<bool()>& cancelled,
                                 const std::function<int()>& elapsedMs, const std::function<void(int)>& sleepMs,
                                 int windowMs = 8000, int retryMs = 500)
{
    WaitResult r;
    const int start = elapsedMs();
    for (;;) {
        ++r.probes;
        if (probe()) {
            r.available = true;
            break;
        }
        r.waitedMs = elapsedMs() - start;
        if (cancelled()) {
            r.cancelled = true;
            break;
        }
        if (r.waitedMs >= windowMs) break;
        sleepMs(retryMs);
    }
    r.waitedMs = elapsedMs() - start;
    return r;
}

} // namespace khensu::service_ready
