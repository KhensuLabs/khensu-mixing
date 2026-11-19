// plugin-copilot/Source/PluginWorkerPool.h
#pragma once
#include <functional>

namespace khensu::copilot {

/// The plugin's owned pool for long background loads (ENG-9): it replaces the
/// std::thread(...).detach() each session refresh used to start.
///
/// The threads live for the process: a load that overruns an instance's
/// shutdown budget (an uninterruptible native decode) must never be joined
/// on the message thread (audit H3 - a join froze the DAW), and must never
/// run code the host has unmapped. So the pool is created once, pins the
/// plugin module the moment it starts its threads, and is deliberately never
/// torn down (allocated as an intentional leak so the Debug CRT dump stays
/// clean). Two workers: one instance's slow load does not hold up another's.
class PluginWorkerPool {
  public:
    static constexpr int kWorkers = 2;

    /// Queues `job`; returns false if the pool could not start (no threads),
    /// in which case the job was not queued.
    static bool submit(std::function<void()> job) noexcept;

    /// Jobs queued or running (tests).
    static int pendingForTesting() noexcept;
};

} // namespace khensu::copilot
