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
#pragma once
#include <functional>

namespace khensu::service_ready {

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
