#pragma once

namespace khensu::ipc {

/// Tri-state view of a polled service's reachability, driven by a stream of
/// boolean "did the last call succeed" ticks (audit ARCH-05).
///
/// A plain `bool isHealthy` (what the plugin tracked before this) can't tell
/// a user who's never launched the desktop app ("service never seen") from
/// one whose desktop app just closed mid-session ("service was there, now
/// isn't") - both collapse to the same "Offline" label, even though the
/// right next action differs (start the app vs. check what happened to it).
///
/// This class owns no lifecycle-management responsibility - it does not
/// spawn, stop, or restart anything. The plugin depends on a desktop-owned
/// service and never takes ownership of its process lifecycle (some DAW
/// hosts sandbox/restrict child-process creation from a plugin, and a
/// shared-supervisor design was explicitly ruled out as higher-risk for the
/// hosted-plugin case - see ARCH-05 in the 2026-07-18 audit). It only makes
/// the *existing* degraded state legible.
enum class ServiceHealthStatus {
    NeverConnected, ///< No tick has ever succeeded since this tracker was created.
    Connected,      ///< The most recent tick succeeded.
    Lost,           ///< At least one tick previously succeeded, but the most recent one(s) failed.
};

/// THREAD_CONTEXT(SingleThreaded) - callers (e.g. the plugin's transport-sync
/// worker thread) own exclusive access; not internally synchronized.
class ServiceHealthTracker {
  public:
    /// Records the outcome of one poll/call and returns the resulting status.
    ServiceHealthStatus recordTick(bool succeeded) noexcept
    {
        if (succeeded) {
            everConnected_ = true;
            consecutiveFailures_ = 0;
            status_ = ServiceHealthStatus::Connected;
        } else {
            ++consecutiveFailures_;
            status_ = everConnected_ ? ServiceHealthStatus::Lost : ServiceHealthStatus::NeverConnected;
        }
        return status_;
    }

    ServiceHealthStatus status() const noexcept { return status_; }

    /// Ticks since the last success (0 while Connected). Lets a caller
    /// distinguish a brief blip from a sustained outage worth surfacing
    /// more insistently, without this class dictating any UI policy itself.
    int consecutiveFailures() const noexcept { return consecutiveFailures_; }

    bool everConnected() const noexcept { return everConnected_; }

  private:
    bool everConnected_ = false;
    int consecutiveFailures_ = 0;
    ServiceHealthStatus status_ = ServiceHealthStatus::NeverConnected;
};

} // namespace khensu::ipc
