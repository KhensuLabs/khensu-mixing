// desktop-app/Source/session/ServiceHealthMonitor.h
#pragma once

namespace khensu {

/// The Python-service health state machine, split out of AppController's timer
/// (audit refactor "Split AppController"). Pure logic: AppController feeds it
/// each health-check observation and acts on the returned decision, so the
/// edge-detection and one-shot rules are unit-testable without a live service.
class ServiceHealthMonitor final {
  public:
    enum class Status { Running, Starting, GaveUp, Stopped };

    struct Decision {
        Status status = Status::Stopped;
        /// Unhealthy -> healthy edge: re-run an analysis that settled to a
        /// C++-only result while the service was down. Fires once per outage.
        bool reanalyze = false;
        /// Retry provenance outbox entries left from an offline period.
        bool dispatchOutbox = false;
        /// First observation of "gave up restarting" in this outage: show the
        /// error + restart prompt exactly once.
        bool notifyGaveUp = false;
        /// Stopped (not given up) and the backoff allows another launch.
        bool attemptRestart = false;
        /// The process is alive but wedged: it answered before, and nothing
        /// has listened on the port for kWedgedProbes probes. Kill and
        /// relaunch it (restartBlocking) - the launch path above never
        /// fires while the process is alive.
        bool restartWedged = false;
    };

    /// Probes (~1.6 s apart, a refused connection answers at once) a service
    /// that answered before may go without a listener before it is restarted.
    /// ~20 s: longer than the service's own exit watchdog (15 s), so a
    /// shutdown that only ran slow exits by itself and takes the normal
    /// relaunch path instead.
    static constexpr int kWedgedProbes = 12;

    /// `portListening` = something listens on the service port (pass true
    /// when that can't be established, so nothing is restarted on a guess).
    Decision onHealthResult(bool serviceResponding, bool processRunning, bool gaveUp, bool canAttemptRestart,
                            bool portListening = true) noexcept
    {
        Decision d;
        if (serviceResponding) {
            d.status = Status::Running;
            d.dispatchOutbox = true;
            d.reanalyze = !healthy_;
            healthy_ = true;
            answeredSinceLaunch_ = true;
            wedgedProbes_ = 0;
            gaveUpNotified_ = false;
            return d;
        }
        healthy_ = false;
        if (processRunning && answeredSinceLaunch_ && !portListening) {
            // Real run 2026-11-14: the service closed its socket mid-shutdown
            // and never exited; the app showed "starting" with no engine
            // until it was closed.
            d.status = Status::Stopped;
            if (++wedgedProbes_ >= kWedgedProbes) {
                d.restartWedged = true;
                wedgedProbes_ = 0;
                answeredSinceLaunch_ = false; // the relaunch is a cold start
            }
        } else if (processRunning) {
            wedgedProbes_ = 0;
            d.status = Status::Starting; // cold start: alive, not yet healthy
        } else if (gaveUp) {
            d.status = Status::GaveUp;
            d.notifyGaveUp = !gaveUpNotified_;
            gaveUpNotified_ = true;
        } else {
            d.status = Status::Stopped;
            d.attemptRestart = canAttemptRestart;
        }
        return d;
    }

    /// The user asked for a manual restart: the next "gave up" may notify again.
    void resetGaveUpNotice() noexcept { gaveUpNotified_ = false; }
    bool isHealthy() const noexcept { return healthy_; }

  private:
    bool healthy_ = false;
    bool gaveUpNotified_ = false;
    bool answeredSinceLaunch_ = false;
    int wedgedProbes_ = 0;
};

} // namespace khensu
