// desktop-app/Source/ui/shared/SessionEpoch.h
#pragma once
#include <cstdint>
#include <optional>

namespace khensu::session_epoch {

/// The session epoch a background task was QUEUED under (see
/// AppController::runInBackground / sessionEpoch()). Set only on pool threads
/// for the duration of a job; empty everywhere else.
inline thread_local std::optional<uint64_t> tlTaskEpoch;

/// RAII: marks the current thread as running a job queued under `epoch`.
struct ScopedTaskEpoch {
    explicit ScopedTaskEpoch(uint64_t epoch) noexcept : previous_(tlTaskEpoch) { tlTaskEpoch = epoch; }
    ~ScopedTaskEpoch() { tlTaskEpoch = previous_; }
    ScopedTaskEpoch(const ScopedTaskEpoch&) = delete;
    ScopedTaskEpoch& operator=(const ScopedTaskEpoch&) = delete;

  private:
    std::optional<uint64_t> previous_;
};

/// Captured into a message-thread callback posted from a background job:
/// `callAsync([self, result, taskEpoch = session_epoch::current()] { ... })`.
inline std::optional<uint64_t> current() noexcept { return tlTaskEpoch; }

/// True unless the job was queued under an older session. Unknown origin
/// (posted from the message thread or a foreign thread) counts as current.
template <typename Controller>
bool stillCurrent(const std::optional<uint64_t>& taskEpoch, const Controller& controller) noexcept
{
    return !taskEpoch.has_value() || *taskEpoch == controller.sessionEpoch();
}

} // namespace khensu::session_epoch
