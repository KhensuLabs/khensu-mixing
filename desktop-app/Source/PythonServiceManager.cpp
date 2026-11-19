#include <khensu/ipc/HttpConnectionLimit.h>
#include "PythonServiceManager.h"
#include <khensu/common/LogRetention.h>
#include <khensu/common/AppPaths.h>
#include <khensu/common/HmacSha256.h>
#include <khensu/common/SecureRandom.h>
#include <algorithm>
#include <khensu/ipc/generated/ApiSchema.h>
#include "PortListeners.h"

// Service JSON field names, generated from resources/api/openapi.json:
// a renamed Python field is a compile error here, not a silent 0.
namespace apif = khensu::api::fields;

namespace khensu {

void PythonServiceManager::setAuthToken(const juce::String& token)
{
    std::lock_guard<std::mutex> lock(tokenMutex_);
    authToken_ = token.trim();
}

juce::String PythonServiceManager::authToken() const
{
    std::lock_guard<std::mutex> lock(tokenMutex_);
    return authToken_;
}

PythonServiceManager::HealthVerdict PythonServiceManager::classifyHealthResponse(const juce::var& json,
                                                                                 const juce::String& token,
                                                                                 const juce::String& nonce)
{
    if (!json.isObject()) return HealthVerdict::Invalid;
    if (json[apif::HealthResponse::version].toString() != kExpectedServiceVersion)
        return json[apif::HealthResponse::service].toString() == "khensu-services" ? HealthVerdict::Stale : HealthVerdict::Invalid;
    // AUDIT-020: the right version string proves nothing - any local process
    // can print it. Only a listener that can read our token file can answer
    // HMAC-SHA256(token, nonce) for a nonce it has never seen.
    if (token.isEmpty() || nonce.isEmpty()) return HealthVerdict::Unverified;
    const std::string expected = HmacSha256::hmacHex(token.toStdString(), nonce.toStdString());
    const std::string proof = json[apif::HealthResponse::proof].toString().trim().toLowerCase().toStdString();
    return HmacSha256::constantTimeEquals(expected, proof) ? HealthVerdict::Verified : HealthVerdict::Unverified;
}

bool PythonServiceManager::verifyServiceIdentity(int timeoutMs) { return isAvailable(timeoutMs); }

bool PythonServiceManager::isAvailable(int timeout_ms)
{
    ipc::ensureHttpConnectionLimit(); // or this probe queues behind long requests (WinINet: 2 per server)
    const ProbeOutcome outcome = [&] {
        // A fresh 32-byte nonce per probe, so a recorded answer can't be replayed.
        const juce::String nonce(SecureRandom::hex(32));
        int statusCode = 0;
        juce::URL url(juce::String(kBaseUrl) + "/health?nonce=" + nonce);
        auto opts = juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress)
                        .withConnectionTimeoutMs(timeout_ms)
                        .withStatusCode(&statusCode);
        auto stream = url.createInputStream(opts);
        if (stream == nullptr) {
            foreignListener_.store(false); // nothing answered (refused, or too slow)
            return ProbeOutcome::NoAnswer;
        }
        if (statusCode != 200) {
            // Something answers HTTP on our port, but not with a /health
            // body - another program owns it (DSK-17).
            foreignListener_.store(true);
            return ProbeOutcome::Rejected;
        }

        const auto json = juce::JSON::parse(stream->readEntireStreamAsString());
        const auto verdict = classifyHealthResponse(json, authToken(), nonce);
        foreignListener_.store(verdict == HealthVerdict::Invalid || verdict == HealthVerdict::Unverified);
        switch (verdict) {
            case HealthVerdict::Verified:
                staleDetected_.store(false);
                return ProbeOutcome::Verified;
            case HealthVerdict::Stale:
                juce::Logger::writeToLog("PythonServiceManager: version handshake mismatch - expected " +
                                         juce::String(kExpectedServiceVersion) + ", got " +
                                         json[apif::HealthResponse::version].toString());
                // A khensu service with the wrong version is a stale build
                // squatting on the port - its route set predates what this app
                // calls, which is exactly how "Could not reach /analyze/..."
                // panels happen (spec §0). Flag it so the next launch attempt
                // terminates it first.
                staleDetected_.store(true);
                return ProbeOutcome::Rejected;
            case HealthVerdict::Unverified:
                // Not provably ours: never adopted, never sent the token, and
                // never killed either (it may not be a khensu process at all).
                juce::Logger::writeToLog("PythonServiceManager: listener on port " + juce::String(kPort) +
                                         " failed the identity challenge - not trusting it");
                return ProbeOutcome::Rejected;
            case HealthVerdict::Invalid:
                break;
        }
        return ProbeOutcome::Rejected;
    }();
    // Who holds the port now. On a verified probe that is the process that
    // proved itself; on a miss, the grace needs that same process to be the
    // port's only listener (a dead service or a new squatter closes the gate).
    const auto pids = (outcome == ProbeOutcome::Rejected) ? std::vector<juce::uint32>{} : listenerPids(kPort);
    const juce::uint32 soleListener = pids.size() == 1 ? pids.front() : 0;
    bool verified = false;
    {
        const std::lock_guard<std::mutex> lock(identityMutex_);
        const bool listenerUnchanged = soleListener != 0 && soleListener == verifiedListenerPid_;
        verified = identityAfterProbe(ipc::HttpServiceBridge::serviceIdentityVerified(), outcome, unansweredProbes_,
                                      listenerUnchanged);
        if (outcome == ProbeOutcome::Verified) verifiedListenerPid_ = soleListener;
        else if (!verified) verifiedListenerPid_ = 0;
        ipc::HttpServiceBridge::setServiceIdentityVerified(verified);
    }
    // The caller asks "did THIS probe verify it" - the grace only keeps the
    // gate open for other requests, it never reports a probe nobody answered
    // as healthy (the health monitor still sees the miss).
    return outcome == ProbeOutcome::Verified;
}

bool PythonServiceManager::identityAfterProbe(bool wasVerified, ProbeOutcome outcome, int& unansweredProbes,
                                              bool listenerUnchanged) noexcept
{
    if (outcome != ProbeOutcome::NoAnswer) {
        unansweredProbes = 0;
        return outcome == ProbeOutcome::Verified;
    }
    if (!wasVerified || !listenerUnchanged) {
        unansweredProbes = 0;
        return false;
    }
    ++unansweredProbes;
    return unansweredProbes <= kIdentityGraceProbes;
}

std::vector<juce::uint32> PythonServiceManager::listenerPids(int port)
{
    const auto raw = tcpListenerPids(port);
    return {raw.begin(), raw.end()};
}

void PythonServiceManager::launchAsync()
{
    // Owned thread (joined in the destructor): if a launch is already in
    // flight the thread is running and startThread() is skipped. This replaces
    // a detached thread that captured raw `this` across a multi-second window.
    if (!isThreadRunning()) startThread();
}

void PythonServiceManager::run()
{
    if (launchIfNeeded()) pumpLogsUntilExit();
}

juce::String PythonServiceManager::findPythonExe() const
{
    if (hasPythonExeOverride_) return pythonExeOverride_;

    // --- Priority 0: installer-staged, hash-verified runtime adjacent to
    // launch_service.py. It must win over a developer venv or PATH Python.
    // If the user ran `python -m venv .venv && pip install -e .` in the
    // local-services directory this is the correct Python - it has all
    // required packages and avoids polluting the system environment.
    {
        const juce::File launcher = findLauncherScript();
        if (launcher.existsAsFile()) {
            auto launcherDir = launcher.getParentDirectory();
#if JUCE_WINDOWS
            auto runtimePy = launcherDir.getChildFile("runtime/python.exe");
            if (runtimePy.existsAsFile()) {
                juce::Logger::writeToLog("PythonServiceManager: using staged runtime at: " + runtimePy.getFullPathName());
                return runtimePy.getFullPathName();
            }
            auto venvPy = launcherDir.getChildFile(".venv/Scripts/python.exe");
#else
            auto runtimePy = launcherDir.getChildFile("runtime/bin/python3");
            if (!runtimePy.existsAsFile()) runtimePy = launcherDir.getChildFile("runtime/bin/python");
            if (runtimePy.existsAsFile()) {
                juce::Logger::writeToLog("PythonServiceManager: using staged runtime at: " + runtimePy.getFullPathName());
                return runtimePy.getFullPathName();
            }
            auto venvPy = launcherDir.getChildFile(".venv/bin/python3");
            if (!venvPy.existsAsFile()) venvPy = launcherDir.getChildFile(".venv/bin/python");
#endif
            if (venvPy.existsAsFile()) {
                juce::Logger::writeToLog("PythonServiceManager: using venv Python at: " + venvPy.getFullPathName());
                return venvPy.getFullPathName();
            }
        }
    }

    // --- Priority 1: absolute paths verified by file existence only.
    // This avoids PATH-based binary planting: we never exec an untrusted
    // binary just to check if Python is there.
#if JUCE_MAC
    // CE-PLT-5: python.org framework builds, then Homebrew (Apple Silicon,
    // then Intel). A Finder-launched app has no shell PATH, so this list -
    // not Priority 2 - is what normally finds Python on a Mac.
    {
        const juce::StringArray absPaths{
            "/Library/Frameworks/Python.framework/Versions/3.13/bin/python3",
            "/Library/Frameworks/Python.framework/Versions/3.12/bin/python3",
            "/Library/Frameworks/Python.framework/Versions/3.11/bin/python3",
            "/opt/homebrew/bin/python3",
            "/usr/local/bin/python3",
        };
        for (const auto& p : absPaths) {
            if (juce::File(p).existsAsFile()) {
                juce::Logger::writeToLog("PythonServiceManager: using Python at: " + p);
                return p;
            }
        }
    }
#endif
#if JUCE_WINDOWS
    {
        juce::String user = juce::SystemStats::getLogonName();
        const juce::StringArray absPaths{
            "C:\\Users\\" + user + "\\AppData\\Local\\Programs\\Python\\Python313\\python.exe",
            "C:\\Users\\" + user + "\\AppData\\Local\\Programs\\Python\\Python312\\python.exe",
            "C:\\Users\\" + user + "\\AppData\\Local\\Programs\\Python\\Python311\\python.exe",
            "C:\\Python313\\python.exe",
            "C:\\Python312\\python.exe",
            "C:\\Python311\\python.exe",
        };
        for (const auto& p : absPaths) {
            if (juce::File(p).existsAsFile()) {
                juce::Logger::writeToLog("PythonServiceManager: using Python at: " + p);
                return p;
            }
        }
    }
#endif

    // --- Priority 2: PATH fallback - probe to confirm it's Python 3.
    // Binary-planting risk exists here, but this path is only reached when
    // no Python is installed to a standard location.
    const juce::StringArray pathNames{
#if JUCE_WINDOWS
        "py", "python3", "python"
#else
        "python3", "python"
#endif
    };
    for (const auto& name : pathNames) {
        juce::ChildProcess probe;
        juce::StringArray args{name, "--version"};
        if (probe.start(args, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr)) {
            juce::String out = probe.readAllProcessOutput();
            probe.waitForProcessToFinish(3000);
            if (out.containsIgnoreCase("Python 3")) {
                juce::Logger::writeToLog("PythonServiceManager: using Python (PATH) at: " + name);
                return name;
            }
        }
    }
    return {};
}

juce::File PythonServiceManager::findLauncherScript() const
{
    if (hasLauncherPathOverride_) return launcherPathOverride_;

#if JUCE_MAC
    // CE-PLT-5: a packaged .app carries the service in Contents/Resources
    // (packaging/macos/build_and_package.sh); the executable is in
    // Contents/MacOS.
    {
        const auto bundled = juce::File::getSpecialLocation(juce::File::currentExecutableFile)
                                 .getParentDirectory()
                                 .getSiblingFile("Resources")
                                 .getChildFile("local-services/launch_service.py");
        if (bundled.existsAsFile()) return bundled;
    }
#endif
    // Walk up from the executable looking for local-services/launch_service.py
    auto dir = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory();
    for (int depth = 0; depth < 8; ++depth) {
        auto candidate = dir.getChildFile("local-services/launch_service.py");
        if (candidate.existsAsFile()) return candidate;
        dir = dir.getParentDirectory();
    }
    return {};
}

void PythonServiceManager::killStaleListener()
{
#if JUCE_WINDOWS
    // Targeted: resolve the PID listening on kPort and terminate it. Guarded
    // by the caller - only reached after /health on this port identified a
    // khensu-services build with a stale version, never an unknown process.
    juce::Logger::writeToLog("PythonServiceManager: terminating stale service on port " + juce::String(kPort));
    juce::ChildProcess killer;
    const juce::String cmd = "powershell -NoProfile -Command \"Get-NetTCPConnection -LocalPort " + juce::String(kPort) +
                             " -State Listen -ErrorAction SilentlyContinue | "
                             "ForEach-Object { Stop-Process -Id $_.OwningProcess -Force }\"";
    if (killer.start(cmd, 0)) killer.waitForProcessToFinish(5000);
#else
    juce::ChildProcess killer;
    const juce::String cmd = "/bin/sh -c \"lsof -ti tcp:" + juce::String(kPort) + " | xargs -r kill -9\"";
    if (killer.start(cmd, 0)) killer.waitForProcessToFinish(5000);
#endif
    staleDetected_.store(false);
}

bool PythonServiceManager::launchIfNeeded()
{
    if (shutdownInProgress_.load()) return false; // restartBlocking() relaunches once it is done
    if (isProcessRunning()) {
        juce::Logger::writeToLog(
            "PythonServiceManager: service process is already running or starting - skipping launch");
        return false;
    }

    // Worker thread owns process_ lifetime: release any dead previous child
    // here (never in shutdown(), which can race the log pump's read).
    {
        std::lock_guard<std::mutex> lock(processMutex_);
        process_.reset();
    }
    shuttingDown_.store(false);

    // Fast pre-check: if something is already healthy on the port, skip launch.
    // 300ms timeout - connection refused returns in <1ms; we only wait this long if
    // the port is actually open but slow (which means it IS our service).
    if (!skipAvailabilityCheckForTesting_ && isAvailable(300)) {
        juce::Logger::writeToLog("PythonServiceManager: verified service already running - adopting it");
        adopted_.store(true);
        launched_.store(true);
        return false;
    }
    adopted_.store(false);

    // A responding-but-stale khensu service blocks the port; the launch below
    // would fail to bind and crash-loop into "gave up". Remove it first.
    if (staleDetected_.load()) killStaleListener();

    // Another program owns the port (DSK-17): our service could never bind,
    // and spawning it anyway only crash-looped into a misleading "check that
    // Python 3 is installed". Count it toward the give-up threshold with the
    // real reason; it is re-probed on every attempt.
    if (!skipAvailabilityCheckForTesting_ && foreignListener_.load()) {
        recordLaunchFailure("port " + juce::String(kPort) + " is in use by another process");
        return false;
    }

    const juce::String pythonExe = findPythonExe();
    if (pythonExe.isEmpty()) {
        recordLaunchFailure("Python 3 not found - deep analysis unavailable");
        return false;
    }

    const juce::File launcher = findLauncherScript();
    if (!launcher.existsAsFile()) {
        recordLaunchFailure("launch_service.py not found - deep analysis unavailable");
        return false;
    }

    auto newProcess = std::make_unique<juce::ChildProcess>();
    // Build quoted command line - juce::ChildProcess on Windows joins StringArray
    // args with spaces without quoting, which breaks paths containing spaces.
    juce::String cmd = "\"" + pythonExe + "\" \"" + launcher.getFullPathName() + "\"";

    // wantStdOut only - not wantStdErr, so we don't block the child when its
    // stderr pipe buffer fills (uvicorn startup warnings, import messages, etc.).
    if (!newProcess->start(cmd, juce::ChildProcess::wantStdOut)) {
        recordLaunchFailure("failed to start child process");
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(processMutex_);
        process_ = std::move(newProcess);
        // Into the kill-on-close job before the launcher can exec into the
        // venv interpreter (AUDIT-040), so the server dies with the app.
        if (job_.adoptNewPythonChildren() == 0)
            juce::Logger::writeToLog("PythonServiceManager: could not place the service in a job object");
    }

    launched_.store(true);
    juce::Logger::writeToLog("PythonServiceManager: service process launched - waiting for /health");
    return true;
}

juce::String PythonServiceManager::lastLaunchFailureReason() const
{
    std::lock_guard<std::mutex> lock(failureMutex_);
    return lastFailureReason_;
}

void PythonServiceManager::recordLaunchFailure(const juce::String& reason)
{
    {
        std::lock_guard<std::mutex> lock(failureMutex_);
        lastFailureReason_ = reason;
    }
    const int failures = consecutiveFailures_.fetch_add(1) + 1;
    juce::Logger::writeToLog("PythonServiceManager: " + reason + " (failure "
                             + juce::String(failures) + "/"
                             + juce::String(kMaxConsecutiveFailures) + ")");
}

void PythonServiceManager::pumpLogsUntilExit()
{
    // Drain the child's stdout into the ring buffer (spec §9.4). The blocking
    // read is unblocked by shutdown() killing the child (pipe closes), so the
    // destructor's shutdown-then-join order terminates this loop promptly.
    char buf[2048];
    for (;;) {
        if (threadShouldExit()) return;

        std::shared_ptr<juce::ChildProcess> proc;
        {
            std::lock_guard<std::mutex> lock(processMutex_);
            proc = process_;
        }
        if (proc == nullptr) return;

        const int n = proc->readProcessOutput(buf, (int)sizeof(buf) - 1);
        if (n <= 0) {
            // Pipe closed: either an intentional shutdown or a crash.
            if (!shuttingDown_.load() && !threadShouldExit()) {
                juce::Logger::writeToLog("PythonServiceManager: service process exited unexpectedly");
                // Only confirmed crashes count toward the failure backoff.
                consecutiveFailures_.fetch_add(1);
                {
                    std::lock_guard<std::mutex> lock(failureMutex_);
                    lastFailureReason_ = "the service process exited unexpectedly";
                }
                writeCrashCapture();
            }
            return;
        }
        buf[n] = '\0';
        appendLogChunk(juce::String::fromUTF8(buf, n));
    }
}

void PythonServiceManager::appendLogChunk(const juce::String& chunk)
{
    std::lock_guard<std::mutex> lock(logMutex_);
    partialLine_ << chunk;
    // Split completed lines out of the accumulator; the trailing fragment
    // (no newline yet) stays in partialLine_ for the next chunk.
    while (true) {
        const int nl = partialLine_.indexOfChar('\n');
        if (nl < 0) break;
        juce::String line = partialLine_.substring(0, nl).trimEnd();
        partialLine_ = partialLine_.substring(nl + 1);
        if (line.isNotEmpty()) {
            logLines_.push_back(std::move(line));
            while (logLines_.size() > kMaxLogLines)
                logLines_.pop_front();
        }
    }
}

juce::StringArray PythonServiceManager::getRecentLogLines() const
{
    juce::StringArray out;
    std::lock_guard<std::mutex> lock(logMutex_);
    for (const auto& l : logLines_)
        out.add(l);
    return out;
}

void PythonServiceManager::writeCrashCapture()
{
    // Persist the tail of the service log so a crash that happened while the
    // user wasn't looking is still diagnosable (spec §9.4).
    // The app's own data directory (DSK-19) - every other file lives under
    // KhensuMixing; this one used to go to a stray %APPDATA%\Khensu\logs.
    auto dir = juce::File(juce::String(AppPaths::dataDirectory())).getChildFile("logs");
    dir.createDirectory();
    const auto stamp = juce::Time::getCurrentTime().formatted("%Y-%m-%d_%H-%M-%S");
    auto file = dir.getChildFile("service-crash-" + stamp + ".log");

    juce::String content;
    content << "Khensu local service exited unexpectedly at " << juce::Time::getCurrentTime().toString(true, true)
            << "\n"
            << "Last " << juce::String((int)kMaxLogLines) << " stdout lines:\n\n";
    for (const auto& l : getRecentLogLines())
        content << l << "\n";
    file.replaceWithText(content);
    juce::Logger::writeToLog("PythonServiceManager: crash capture written to " + file.getFullPathName());
    logs::pruneOldLogs(dir, "service-crash-*.log", logs::kKeepCrashLogs); // SHIP-7
}

void PythonServiceManager::shutdown()
{
    // Kills the child but deliberately does NOT reset process_: the log-pump
    // thread may hold the raw pointer across a blocking read, so ownership
    // is only ever released on the worker thread (launchIfNeeded) or after
    // the thread is joined (destructor). shuttingDown_ stays true until the
    // next launch so a pump that observes the closed pipe late doesn't
    // mistake this for a crash.
    shuttingDown_.store(true);
    shutdownInProgress_.store(true);
    const struct Done {
        std::atomic<bool>& flag;
        ~Done() { flag.store(false); }
    } done{shutdownInProgress_};
    // DSK-06: the mutex guards only the pointer copy. Holding it across the
    // /shutdown request (1.2 s) and the waits (up to ~10 s) froze the message
    // thread, which takes it in isProcessRunning() on every health tick.
    std::shared_ptr<juce::ChildProcess> proc;
    {
        std::lock_guard<std::mutex> lock(processMutex_);
        proc = process_;
    }
    const bool ownChild = proc != nullptr && launched_.load();
    // An adopted service used to be left running forever (AUDIT-040). The
    // identity gate keeps /shutdown (it carries the token) from going to a
    // listener that never passed the challenge.
    if (ownChild || adopted_.load()) {
        juce::Logger::writeToLog("PythonServiceManager: requesting graceful service shutdown");
        ipc::HttpServiceBridge bridge(kBaseUrl, 1200, 0);
        const auto graceful = bridge.requestGracefulShutdown();
        if (graceful.is_err())
            juce::Logger::writeToLog("PythonServiceManager: graceful shutdown request failed: "
                                     + juce::String(graceful.error().message));
    }
    if (ownChild) {
        // The launcher exec()s into the venv interpreter, so the process we
        // spawned may already be gone while the server still runs - wait on
        // the whole job, then kill it.
        const bool exited = proc->waitForProcessToFinish(3500) && job_.waitUntilEmpty(3500);
        if (!exited) {
            juce::Logger::writeToLog("PythonServiceManager: graceful shutdown deadline elapsed; killing child");
            proc->kill();
            job_.terminateAll();
            proc->waitForProcessToFinish(3000);
        }
    }
    adopted_.store(false);
    launched_.store(false);
    {
        const std::lock_guard<std::mutex> lock(identityMutex_);
        unansweredProbes_ = 0;
        verifiedListenerPid_ = 0;
        ipc::HttpServiceBridge::setServiceIdentityVerified(false);
    }
}

void PythonServiceManager::restartBlocking()
{
    if (restartInFlight_.exchange(true)) return; // a restart is already running
    const struct Reset {
        std::atomic<bool>& flag;
        ~Reset() { flag.store(false); }
    } reset{restartInFlight_};
    const bool wasAdopted = adopted_.load();
    shutdown();
    // The log pump exits once it sees the closed pipe; launchAsync() is a
    // no-op while the old thread is still running.
    waitForThreadToExit(5000);
    // An adopted service exits on its own after answering /shutdown; give it
    // time to free the port so the relaunch doesn't simply re-adopt it.
    if (wasAdopted) {
        const auto deadline = juce::Time::getMillisecondCounter() + 4000;
        while (juce::Time::getMillisecondCounter() < deadline && isAvailable(250))
            juce::Thread::sleep(150);
    }
    notifyHealthy();
    launchAsync();
}

bool PythonServiceManager::isProcessRunning() const noexcept
{
    // Mutex-guarded pointer copy: the UI timer reads this while the launch
    // thread may be creating/resetting process_. The lock is never held
    // across blocking work (DSK-06), so this cannot stall the message thread.
    std::shared_ptr<juce::ChildProcess> proc;
    {
        std::lock_guard<std::mutex> lock(processMutex_);
        proc = process_;
    }
    return proc != nullptr && proc->isRunning();
}

bool PythonServiceManager::canAttemptRestart() noexcept
{
    if (consecutiveFailures_.load() >= kMaxConsecutiveFailures) return false;

    const auto now = static_cast<juce::int64>(juce::Time::getMillisecondCounter());
    if (now < nextAttemptAllowedMs_.load()) return false;

    // Use a separate launch-attempt counter for backoff scheduling so that
    // consecutiveFailures_ only reflects confirmed crashes (process died without
    // ever becoming healthy), not preemptive increments for each scheduled
    // retry.  This keeps the "restarting (attempt N)" label accurate.
    const int attempts = launchAttempts_.fetch_add(1);
    // Exponential backoff capped at 30s: 1s, 2s, 4s, 8s, 16s, 30s, 30s, ...
    const juce::int64 backoffMs = std::min<juce::int64>(30000, 1000LL << std::min(attempts, 5));
    nextAttemptAllowedMs_.store(now + backoffMs);
    return true;
}

} // namespace khensu
