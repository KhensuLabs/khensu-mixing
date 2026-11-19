#pragma once

// Thread context documentation system.
//
// THREAD_CONTEXT() is a pure compile-time annotation - zero runtime cost.
// Apply it to every class declaration and every non-trivial method.
//
// Allowed contexts:
//   AudioThread   - real-time safe only. No allocs, locks, I/O, IPC, ML inference.
//   UIThread      - JUCE message thread. Non-blocking. UI updates only.
//   WorkerThread  - Background threads. May block, allocate, do I/O, call Python service.
//   AnyThread     - Genuinely thread-safe (lock-free atomics, const reads, etc.).

namespace khensu {

enum class ThreadContext {
    AudioThread,
    UIThread,
    WorkerThread,
    AnyThread,
};

} // namespace khensu

/// Annotation macro. Documents the allowed thread context for a class or function.
/// Has no runtime effect - purely for documentation and grep-based auditing.
/// Usage: THREAD_CONTEXT(khensu::ThreadContext::AudioThread)
#define THREAD_CONTEXT(ctx)
