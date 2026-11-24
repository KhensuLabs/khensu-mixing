#pragma once
#include <khensu/ipc/DeepAnalysisRequest.h>
#include <khensu/ipc/HttpServiceBridge.h>
#include <khensu/analysis/SectionAnalysis.h>
#include <khensu/common/AppPaths.h>
#include <juce_core/juce_core.h>
#include <chrono>
#include <functional>
#include <memory>
#include <thread>

namespace khensu::ipc {

// UI-owned bridge calls must fail promptly when the local service is wedged.
// Long render/encode operations opt in by constructing HttpServiceBridge with
// a larger timeout; a forgotten call site gets this conservative default.
constexpr int kReadDeadlineMs = 30000;

/// Reads the whole body, cancelling the stream once `deadlineMs` elapses
/// (then *timedOut is set and the truncated body must not be parsed).
///
/// The deadline is enforced by ONE process-wide deadline thread (audit §8.3)
/// shared by every request - it used to start a watchdog thread per request,
/// 5-10 thread creations a second from the pollers alone.
juce::String readAllBounded(juce::InputStream& stream, int deadlineMs = kReadDeadlineMs, bool* timedOut = nullptr);

/// Watches a blocking stream read for caller cancellation (CORE-14): every
/// `intervalMs` the shared deadline thread asks `shouldCancel` and, once it
/// returns true, cancels the stream so a read blocked inside WinINet returns
/// immediately instead of after the connection timeout (up to 180 s for the
/// chat stream). `shouldCancel` runs on the deadline thread - it must be
/// thread-safe (an atomic flag read) and cheap. The destructor stops the
/// watch and waits for an in-flight check, so the stream may be destroyed
/// right after the CancelWatch.
class CancelWatch {
  public:
    CancelWatch(juce::InputStream& stream, std::function<bool()> shouldCancel, int intervalMs = 200);
    ~CancelWatch();
    CancelWatch(const CancelWatch&) = delete;
    CancelWatch& operator=(const CancelWatch&) = delete;

    /// True once the watch cancelled the stream.
    bool cancelled() const noexcept;

  private:
    struct State;
    std::shared_ptr<State> state_;
};

/// Stops and joins the shared deadline thread (HttpServiceBridge::
/// shutdownBackgroundWorkers). A later request lazily starts a fresh one; the
/// scheduler is heap-held, never a static with a joining destructor.
void shutdownDeadlineScheduler();

/// Sized per-call ceilings for SYNCHRONOUS heavy routes (CORE-03 / WIRE-03).
/// JUCE applies the timeout as WinINet's connect/receive/send timeout and
/// FastAPI sends no headers until the handler returns, so this bounds the
/// server's compute time - the 30 s constructor default failed these calls
/// with a false error while the service was still working.
namespace heavy_timeout {
inline constexpr int kAnalyzeExtendedMs = 10 * 60 * 1000; ///< full /analyze/extended pass
inline constexpr int kAlbumMs = 10 * 60 * 1000;           ///< decodes every track of an album
inline constexpr int kPitchModelMs = 6 * 60 * 1000;       ///< basic-pitch alone has a 120 s floor + worker start
inline constexpr int kHumanizeMs = 10 * 60 * 1000;        ///< up to 4 scans + 3 renders / multi-stem renders
inline constexpr int kCodecAuditMs = 5 * 60 * 1000;       ///< 3 ffmpeg encode/decode round trips
inline constexpr int kRenderMs = 5 * 60 * 1000;           ///< offline renders (delta solo, pitch / clip gain)
inline constexpr int kStemAnalysisMs = 5 * 60 * 1000;     ///< multi-file / stem-aware analyses
/// A whole-track pitch track (CREPE / pyin): ~8 s alone, but it queues behind
/// the DSP pool's other scans, and 30 s failed it while it waited (RUN-13).
inline constexpr int kPitchTrackMs = 3 * 60 * 1000;
} // namespace heavy_timeout

// --- Small helpers for the per-endpoint wrappers ---------------------------

/// Re-wrap a failed request's BridgeError in the caller's result type.
template <typename T>
khensu::Result<T, BridgeError> forwardError(const khensu::Result<juce::var, BridgeError>& failed)
{
    return khensu::Result<T, BridgeError>::err(failed.error());
}

/// A response that parsed as JSON but lacks the shape the caller needs.
template <typename T> khensu::Result<T, BridgeError> shapeError(std::string message)
{
    return khensu::Result<T, BridgeError>::err({BridgeError::Kind::ParseError, std::move(message)});
}

/// A status-only call's outcome: success, or the request's BridgeError (kept
/// so callers can tell a 403 "disabled in Settings" from an unreachable service).
inline khensu::Result<void, BridgeError> statusResult(const khensu::Result<juce::var, BridgeError>& r)
{
    if (r.is_err()) return khensu::Result<void, BridgeError>::err(r.error());
    return khensu::Result<void, BridgeError>::ok();
}

/// Body must be a JSON object.
inline BridgeRequestOptions objectResponse(int timeoutMs = 0)
{
    BridgeRequestOptions o;
    o.requireObject = true;
    o.timeoutMs = timeoutMs;
    return o;
}

/// A heavy synchronous call: at least `heavyMs`, or the bridge's own timeout
/// when a caller constructed it with a longer one.
inline int heavyTimeoutMs(int heavyMs, int bridgeTimeoutMs) { return heavyMs > bridgeTimeoutMs ? heavyMs : bridgeTimeoutMs; }

/// Job-status poll: short connect timeout, any non-200 is "Could not poll".
inline BridgeRequestOptions pollOptions(int readDeadlineMs = 0)
{
    BridgeRequestOptions o;
    o.timeoutMs = 5000;
    o.readDeadlineMs = readDeadlineMs;
    o.non200IsUnavailable = true;
    return o;
}

/// Status-only call: succeeds on 2xx without reading the body.
inline BridgeRequestOptions statusOnly(int timeoutMs = 0)
{
    BridgeRequestOptions o;
    o.parseBody = false;
    o.timeoutMs = timeoutMs;
    return o;
}

inline BridgeRequestOptions withTimeout(int timeoutMs)
{
    BridgeRequestOptions o;
    o.timeoutMs = timeoutMs;
    return o;
}

/// {"job_id": ...} from a job-start response.
inline khensu::Result<juce::String, BridgeError> extractJobId(const khensu::Result<juce::var, BridgeError>& r)
{
    if (r.is_err()) return forwardError<juce::String>(r);
    auto* obj = r.value().getDynamicObject();
    if (obj == nullptr || !obj->hasProperty("job_id")) return shapeError<juce::String>("Response missing job_id");
    return khensu::Result<juce::String, BridgeError>::ok(obj->getProperty("job_id").toString());
}

inline juce::File tokenFile()
{
    return juce::File(juce::String(AppPaths::dataDirectory()) + "/token.txt");
}

inline juce::String readTokenFromDisk()
{
    return tokenFile().loadFileAsString().trim();
}

} // namespace khensu::ipc
