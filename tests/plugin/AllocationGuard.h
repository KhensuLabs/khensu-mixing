// tests/plugin/AllocationGuard.h
// Heap-allocation detector for real-time-audio tests (audit CQ-03). The test
// binary replaces global operator new/delete with counting versions gated on
// a thread_local flag, so ONLY the thread running processBlock is watched -
// Catch2, JUCE timers, and worker threads allocate freely.
//
// The C runtime's malloc / calloc / realloc are counted as well on Windows
// (VER-5): JUCE's HeapBlock bypasses operator new. With the static C runtime
// (SHIP-1) they are seen through the heap calls the CRT makes.
//
// The overrides never fail an allocation: they count violations and let the
// allocation proceed, so a failing test reports counts instead of crashing.
#pragma once
#include <atomic>
#include <cstddef>

namespace khensu::test {

// Defined in AllocationGuard.cpp alongside the operator new/delete overrides.
extern thread_local bool allocationGuardArmed;
extern std::atomic<long> allocationViolations;
/// How many import-table slots of the malloc family (or HeapAlloc / HeapReAlloc)
/// were redirected at start-up
/// (0 off Windows, or when the exe imports none).
extern const int crtAllocatorHookedSlots;

/// RAII: arms the guard on the current thread for its lifetime.
struct ScopedAllocationGuard {
    ScopedAllocationGuard() { allocationGuardArmed = true; }
    ~ScopedAllocationGuard() { allocationGuardArmed = false; }
    ScopedAllocationGuard(const ScopedAllocationGuard&) = delete;
    ScopedAllocationGuard& operator=(const ScopedAllocationGuard&) = delete;
};

inline long violationCount() { return allocationViolations.load(); }
inline void resetViolations() { allocationViolations.store(0); }

} // namespace khensu::test
