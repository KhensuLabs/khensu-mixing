// desktop-app/Source/IBackgroundWork.h
#pragma once
#include <cstdint>
#include <functional>

namespace khensu {

/// The background-work slice of the controller (ENG-1 step 2): what a
/// component needs to follow the session-epoch contract and nothing else.
/// Implemented by AppController (via IAppController) on top of its
/// BackgroundRunner. Components that only fetch in the background take
/// `IBackgroundWork&` instead of `IAppController&`, so they don't compile
/// against (or rebuild with) the whole controller.
class IBackgroundWork {
  public:
    virtual ~IBackgroundWork() = default;

    /// Short background work (panel fetches). Joined at shutdown.
    virtual void runInBackground(std::function<void()> job) = 0;
    /// Long-lived background work (render/agent polling loops, chat streams):
    /// a separate owned pool so it can't starve short panel fetches. Joined
    /// at shutdown, unlike the detached threads it replaces.
    virtual void runLongInBackground(std::function<void()> job) = 0;
    /// Changes whenever the active session changes; see session_epoch.
    virtual uint64_t sessionEpoch() const noexcept = 0;
    /// True once teardown begins; long loops poll it so the shutdown drain
    /// doesn't wait on them.
    virtual bool isShuttingDown() const noexcept = 0;
};

} // namespace khensu
