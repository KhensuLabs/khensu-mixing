// desktop-app/Source/ui/Dashboard/AutoScanCompletion.h
// Completion contract for a Dashboard panel's auto-run scan (results gate):
// AppController withholds findings until the main analysis AND the scans the
// Dashboard auto-starts after it have finished, so each auto-run must say
// when it ends - success, error, unreachable service or a superseded session.
//
//   bool autoRun(std::function<void()> onDone)
//   {
//       if (!button_.isEnabled()) return false;
//       return autoScan_.begin(std::move(onDone), [this] { runScan(); },
//                              [this] { return !button_.isEnabled(); });
//   }
//   ...in the scan's async result callback, right after the busy state is
//   reset (before any stale-session early return):  self->autoScan_.finish();
//
// begin() returns true only if the scan went asynchronous; `onDone` then
// fires exactly once, from finish(). It never fires synchronously from
// begin(), and a manual (button) run - begin() not involved - never fires it.
//
// request() (DSK-03) is what autoRun() should call: when a scan is ALREADY in
// flight (a manual run, or the previous session's scan whose result the epoch
// check will drop) it does not silently skip - it queues one re-run that
// starts right after the in-flight scan finishes, and reports `onDone` when
// that re-run ends. Without it a session switch mid-scan let the results gate
// open with the previous session's rows still in the panel.
//
//   bool autoRun(std::function<void()> onDone)
//   {
//       return autoScan_.request(std::move(onDone), [this] { runScan(); },
//                                [this] { return !button_.isEnabled(); });
//   }
//
// reset() (session change) drops a queued re-run and a pending report.
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include <functional>
#include <memory>
#include <utility>

namespace khensu {

class AutoScanCompletion final {
  public:
    /// Starts an auto-run: `start()` runs the scan; `isRunning()` reports
    /// whether it went asynchronous. Returns true when `onDone` is pending.
    template <typename Start, typename IsRunning>
    bool begin(std::function<void()> onDone, Start&& start, IsRunning&& isRunning)
    {
        if (pending_) return false; // one auto-run at a time
        pending_ = std::move(onDone);
        inStart_ = true;
        start();
        inStart_ = false;
        if (!pending_) return false; // ended synchronously: nothing started
        if (!isRunning()) {
            pending_ = nullptr;
            return false;
        }
        return true;
    }

    /// The scan ended. Fires a pending auto-run's `onDone` once; a no-op for
    /// manual runs. Inside begin() it only cancels (no synchronous callback).
    void finish()
    {
        if (inStart_) {
            pending_ = nullptr;
            return;
        }
        if (auto done = std::exchange(pending_, nullptr)) done();
        startQueuedSoon();
    }

    bool pending() const noexcept { return static_cast<bool>(pending_); }

    /// begin(), or - when a scan is already running - queue one re-run to
    /// start once it finishes. Returns true when `onDone` will fire.
    template <typename Start, typename IsRunning>
    bool request(std::function<void()> onDone, Start start, IsRunning isRunning)
    {
        if (!isRunning() && !pending_) return begin(std::move(onDone), start, isRunning);
        // A newer request replaces an older queued one; the older one's
        // report still fires (its batch token is stale by now - harmless).
        if (auto older = std::exchange(queued_, nullptr)) older->done();
        queued_ = std::make_shared<Queued>();
        queued_->done = std::move(onDone);
        queued_->start = [start]() mutable { start(); };
        queued_->isRunning = [isRunning]() mutable { return isRunning(); };
        return true;
    }

    /// Session change: forget a queued re-run and any pending report (both
    /// belong to the previous session's batch, which the gate no longer
    /// counts).
    void reset()
    {
        queued_.reset();
        pending_ = nullptr;
    }

    bool hasQueuedRerun() const noexcept { return queued_ != nullptr; }

    /// Tests: run deferred work synchronously instead of via callAsync.
    void setDeferrer(std::function<void(std::function<void()>)> deferrer) { deferrer_ = std::move(deferrer); }

    /// Called from finish(): starts the queued re-run after the current
    /// result callback has returned (so the old run's tail cannot overwrite
    /// the new run's busy state).
    void startQueuedSoon()
    {
        if (!queued_) return;
        auto job = [alive = std::weak_ptr<bool>(alive_), this] {
            if (alive.expired()) return;
            startQueuedNow();
        };
        if (deferrer_)
            deferrer_(job);
        else
            juce::MessageManager::callAsync(job);
    }

  private:
    struct Queued {
        std::function<void()> done;
        std::function<void()> start;
        std::function<bool()> isRunning;
    };

    void startQueuedNow()
    {
        auto q = std::exchange(queued_, nullptr);
        if (!q) return;
        if (q->isRunning()) { // another (manual) run started meanwhile: wait for it
            queued_ = q;
            return;
        }
        if (!begin(q->done, q->start, q->isRunning) && q->done) q->done(); // never went async: report now
    }

    std::function<void()> pending_;
    bool inStart_ = false;
    std::shared_ptr<Queued> queued_;
    std::function<void(std::function<void()>)> deferrer_;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

} // namespace khensu
