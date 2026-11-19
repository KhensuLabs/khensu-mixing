#pragma once
#include <JuceHeader.h>
#include <khensu/ipc/HttpServiceBridge.h>
#include "ServiceProcessJob.h"
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

namespace khensu {

/// Manages the lifecycle of the local Python analysis service.
/// Launched as a child process on app startup; killed on app shutdown.
///
/// Threading: the launch runs on an owned juce::Thread joined in the
/// destructor (a detached thread previously captured a raw `this` across a
/// multi-second window), and `process_` is mutex-guarded because the UI timer
/// reads it while the launch thread creates/resets it. After a successful
/// launch the same thread stays alive pumping the child's stdout into the
/// log ring buffer until the process exits or the app shuts down.
class PythonServiceManager : private juce::Thread {
  public:
    static constexpr int kPort = 47239;
    static constexpr const char* kBaseUrl = ipc::HttpServiceBridge::kDefaultBaseUrl;

    /// Exact service protocol version this build of the desktop app can talk
    /// to. Must track
    /// local-services/khensu_services/version.py - bump both together.
    /// A /health response that identifies as khensu-services but fails this
    /// check is a STALE service process (old build still bound to the port):
    /// the diagnosed root cause of "Could not reach /analyze/..." panels, since
    /// its routes predate the ones this app calls (spec §0).
    static constexpr const char* kExpectedServiceVersion = "0.2.104";

    PythonServiceManager() : juce::Thread("khensu-service-launch") {}
    ~PythonServiceManager() override
    {
        // Order matters: killing the child closes its stdout pipe, which
        // unblocks the log-pump read so the thread can actually be joined.
        signalThreadShouldExit();
        shutdown();
        stopThread(8000);
    }

    /// Fire-and-forget: runs launchIfNeeded() on the owned worker thread.
    /// Returns immediately - never blocks the UI thread.
    void launchAsync();

    /// Stops the service: asks it to exit via /shutdown - our own child AND
    /// an adopted (already-running, identity-verified) one (AUDIT-040) - then
    /// hard-kills our child and everything in its job object. Safe to call
    /// before launchAsync() completes. Blocks for up to ~7 s: never call it
    /// on the message thread except during teardown.
    void shutdown();

    /// shutdown(), wait for the old process/port to go away, reset the
    /// backoff and launch again. BLOCKING (several seconds) - run it on a
    /// background pool, never the message thread (AUDIT-040: the UI froze for
    /// up to ~7.7 s on "Restart Service").
    void restartBlocking();

    /// True once the child process has been spawned, or a running verified
    /// service was adopted.
    bool weStartedIt() const noexcept { return launched_.load(); }

    /// The bearer token this app wrote to token.txt. It is the HMAC key the
    /// /health identity challenge is checked against (AUDIT-020); without it
    /// no listener is ever trusted. Thread-safe.
    void setAuthToken(const juce::String& token);

    /// GET /health?nonce=<fresh CSPRNG hex>: true only if the listener runs
    /// kExpectedServiceVersion AND returns proof == HMAC-SHA256(token, nonce)
    /// - i.e. it can read our token file. Publishes the result to
    /// HttpServiceBridge::setServiceIdentityVerified(), so the token is only
    /// ever sent to a verified service. Used by the launch path and the
    /// periodic health probe. Blocking; call off the message thread.
    bool verifyServiceIdentity(int timeoutMs);

    /// How a /health body compares to what this app expects.
    enum class HealthVerdict {
        Verified,   ///< expected version and a correct proof
        Stale,      ///< a khensu service with the wrong version (old build on the port)
        Unverified, ///< right version but missing/wrong proof - not provably ours
        Invalid,    ///< not a /health body at all
    };
    /// Pure classification of a parsed /health response (unit-testable).
    static HealthVerdict classifyHealthResponse(const juce::var& json, const juce::String& token,
                                                const juce::String& nonce);

    /// What one /health probe established about the listener.
    enum class ProbeOutcome {
        Verified, ///< answered and proved it is our service
        Rejected, ///< answered, but not provably ours (stale, foreign, bad proof)
        NoAnswer, ///< nothing answered within the timeout (refused or slow)
    };
    /// Unanswered probes a verified service may miss before the identity gate
    /// closes (~1.6 s apart). One slow /health - the service busy decoding -
    /// used to close the process-wide gate, failing every request in that
    /// window with "service identity not verified": auto stem separation gave
    /// up and first analyses settled C++-only.
    ///
    /// The grace only applies while the port is still held by exactly the
    /// process that last proved itself (same PID, sole listener). If the
    /// verified service died, or anything else now listens on the port - a
    /// squatter that accepts and stalls to ride out the grace - the gate
    /// closes at once, so the token never reaches it.
    static constexpr int kIdentityGraceProbes = 2;
    /// Pure gate decision (unit-testable): the verified state after a probe.
    /// `unansweredProbes` is the running count of consecutive NoAnswer
    /// probes; `listenerUnchanged` = the port's sole listener is still the
    /// process that last verified (false when it can't be established).
    static bool identityAfterProbe(bool wasVerified, ProbeOutcome outcome, int& unansweredProbes,
                                   bool listenerUnchanged) noexcept;

    /// PIDs of every process listening on 127.0.0.1/0.0.0.0:`port` (TCP,
    /// IPv4). Empty when nothing listens or the table can't be read (non-
    /// Windows builds: always empty, so a missed probe gets no grace there).
    static std::vector<juce::uint32> listenerPids(int port);

    /// True if the child process is currently running. Never blocks on a
    /// shutdown in progress (DSK-06): the message thread calls it every
    /// health tick, and shutdown() no longer holds the process mutex across
    /// its HTTP request and waits.
    bool isProcessRunning() const noexcept;

    /// True when the last probe found a listener on kPort that is NOT a
    /// khensu service we can verify (another program owns the port). The
    /// launch is then skipped - our service could never bind - and the UI
    /// says "port in use by another process" instead of blaming Python
    /// (DSK-17).
    bool portInUseByOtherProcess() const noexcept { return foreignListener_.load(); }
    /// The reason recorded with the last launch failure ("" when none).
    juce::String lastLaunchFailureReason() const;

    // ---- Crash-loop backoff ----------------------------------------------
    // Without this, a service that crashes immediately after every launch
    // (missing dependency, bad venv, etc) gets re-spawned on every ~1.6s
    // health-check tick forever - hammering the machine with child processes
    // and spamming logs instead of surfacing a clear "this isn't working"
    // signal to the user.
    static constexpr int kMaxConsecutiveFailures = 8;

    /// Call before launchAsync() when the service is down. Returns false
    /// (and does NOT count as an attempt) while a backoff delay is still in
    /// effect, or once kMaxConsecutiveFailures has been reached. Each true
    /// return increments the failure count and schedules the next backoff.
    bool canAttemptRestart() noexcept;

    /// True once canAttemptRestart() has been refused kMaxConsecutiveFailures
    /// times in a row - auto-restart has given up.
    bool hasGivenUp() const noexcept { return consecutiveFailures_.load() >= kMaxConsecutiveFailures; }

    /// How many consecutive launch failures have been recorded so far - lets
    /// the UI show "restarting (attempt N)" instead of a static "starting"
    /// label that gives no sign anything unusual is happening during a
    /// crash-loop.
    int consecutiveFailureCount() const noexcept { return consecutiveFailures_.load(); }

    /// Resets the failure/backoff state - call once the service is confirmed
    /// reachable again via /health.
    void notifyHealthy() noexcept
    {
        consecutiveFailures_.store(0);
        launchAttempts_.store(0);
    }

    /// True if the last /health probe got a response from a khensu service
    /// whose version fails kExpectedServiceVersion - i.e. a stale process is
    /// squatting on the port. The next launchIfNeeded() will terminate it
    /// before spawning the current build.
    bool staleServiceDetected() const noexcept { return staleDetected_.load(); }

    // ---- Service log (spec §9.4) -------------------------------------------
    /// Most recent stdout lines from the child service (capped ring buffer),
    /// oldest first. Safe from any thread.
    juce::StringArray getRecentLogLines() const;

    static constexpr size_t kMaxLogLines = 500;

    /// The interpreter / launcher script a launch would use, without
    /// launching anything - the headless CLI (HeadlessCli.cpp) runs
    /// `python -m khensu_services.cli` with the same interpreter.
    juce::String resolvedPythonExe() const { return findPythonExe(); }
    juce::File resolvedLauncherScript() const { return findLauncherScript(); }

    // ---- Test-only seams (audit ARCH-06) ---------------------------------
    // launchIfNeeded()'s three pre-spawn failure branches (missing Python,
    // missing launcher script, process-start failure) previously had no way
    // to be driven deterministically: findPythonExe()/findLauncherScript() do
    // a real filesystem/PATH search, and the normal entry point only runs on
    // an async worker thread. These let a test force each branch directly.

    /// Forces findPythonExe() to return `path` (may be empty, to simulate
    /// "Python not found") instead of searching the filesystem/PATH.
    void setPythonExeOverrideForTesting(const juce::String& path)
    {
        pythonExeOverride_ = path;
        hasPythonExeOverride_ = true;
    }

    /// Forces findLauncherScript() to return `path` (may be a non-existent
    /// File, to simulate "launcher script not found") instead of walking the
    /// directory tree for it.
    void setLauncherPathOverrideForTesting(const juce::File& path)
    {
        launcherPathOverride_ = path;
        hasLauncherPathOverride_ = true;
    }

    /// Runs launchIfNeeded() synchronously on the calling thread instead of
    /// via the async worker thread launchAsync() starts - deterministic for
    /// tests that don't want to poll for the real thread to reach a state.
    bool launchIfNeededForTesting() { return launchIfNeeded(); }

    /// Skips the real isAvailable(300) HTTP probe launchIfNeeded() otherwise
    /// makes first (checking whether something's already listening on
    /// kPort). Irrelevant to testing the pre-spawn failure branches below it,
    /// and each real probe attempt costs ~2s in practice (Windows HTTP-stack
    /// proxy-detection overhead, not a bug in this class) - multiplied by a
    /// give-up-threshold loop, that's real test-suite time.
    void setSkipAvailabilityCheckForTesting() { skipAvailabilityCheckForTesting_ = true; }

  private:
    void run() override;

    bool launchIfNeeded();    // called only from the owned worker thread
    void pumpLogsUntilExit(); // called only from the owned worker thread
    void appendLogChunk(const juce::String& chunk);
    void writeCrashCapture();
    void recordLaunchFailure(const juce::String& reason);
    juce::String findPythonExe() const;
    juce::File findLauncherScript() const;
    bool isAvailable(int timeout_ms);

    juce::String authToken() const;

    /// Terminate whatever khensu service is listening on kPort (stale build
    /// from a previous install/run). Only ever called after /health on that
    /// port identified itself as "khensu-services" with a wrong version -
    /// never against an arbitrary unknown process.
    void killStaleListener();

    mutable std::mutex processMutex_; ///< guards the process_ pointer (held only briefly - DSK-06)
    // Shared so shutdown() can work on its own reference with the mutex
    // released; the worker thread still owns replacing it.
    std::shared_ptr<juce::ChildProcess> process_;
    std::atomic<bool> shutdownInProgress_{false}; ///< a launch must not start mid-shutdown
    std::atomic<bool> foreignListener_{false};
    std::mutex identityMutex_;  ///< one gate decision at a time (launch thread + health poll)
    int unansweredProbes_ = 0;  ///< guarded by identityMutex_
    juce::uint32 verifiedListenerPid_ = 0; ///< port owner at the last verified probe; guarded by identityMutex_
    mutable std::mutex failureMutex_;
    juce::String lastFailureReason_;
    std::atomic<bool> launched_{false};
    std::atomic<bool> adopted_{false}; ///< running service found on the port, not our child
    std::atomic<bool> restartInFlight_{false};
    ServiceProcessJob job_;            ///< guarded by processMutex_
    mutable std::mutex tokenMutex_;
    juce::String authToken_;
    std::atomic<bool> staleDetected_{false};
    std::atomic<bool> shuttingDown_{false};
    std::atomic<int> consecutiveFailures_{0};
    std::atomic<int> launchAttempts_{0}; ///< counts canAttemptRestart() calls (for backoff)
    std::atomic<juce::int64> nextAttemptAllowedMs_{0};

    mutable std::mutex logMutex_; ///< guards logLines_ + partialLine_
    std::deque<juce::String> logLines_;
    juce::String partialLine_;

    bool hasPythonExeOverride_ = false;
    juce::String pythonExeOverride_;
    bool hasLauncherPathOverride_ = false;
    juce::File launcherPathOverride_;
    bool skipAvailabilityCheckForTesting_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PythonServiceManager)
};

} // namespace khensu
