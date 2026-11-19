// tests/plugin/AllocationGuard.cpp
// Global operator new/delete replacements for the allocation detector
// (audit CQ-03). Linking this TU into the test binary replaces the operators
// binary-wide (ODR: user-provided definitions take precedence); they defer to
// malloc/free and only COUNT when the current thread's guard is armed.
// On Windows the CRT's malloc family is counted too (VER-5, below).
#include "AllocationGuard.h"

#include <cstdlib>
#include <new>

namespace khensu::test {
thread_local bool allocationGuardArmed = false;
std::atomic<long> allocationViolations{0};
} // namespace khensu::test

namespace {
void* countedAlloc(std::size_t n)
{
    if (khensu::test::allocationGuardArmed) khensu::test::allocationViolations.fetch_add(1);
    if (n == 0) n = 1;
    if (void* p = std::malloc(n)) return p;
    throw std::bad_alloc{};
}
} // namespace

void* operator new(std::size_t n) { return countedAlloc(n); }
void* operator new[](std::size_t n) { return countedAlloc(n); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept
{
    if (khensu::test::allocationGuardArmed) khensu::test::allocationViolations.fetch_add(1);
    return std::malloc(n == 0 ? 1 : n);
}
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept
{
    if (khensu::test::allocationGuardArmed) khensu::test::allocationViolations.fetch_add(1);
    return std::malloc(n == 0 ? 1 : n);
}

void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }

// ---- VER-5: the C runtime's allocator --------------------------------------
// JUCE's HeapBlock (AudioBuffer, Array, MidiBuffer growth) calls malloc /
// calloc / realloc directly, which the operator new replacements above never
// see. On Windows the test exe reaches the CRT allocator through its import
// address table, so at start-up the table's malloc-family entries are pointed
// at counting wrappers. Every call made by code linked into this exe (the
// plugin sources, JUCE, the tests) goes through them; allocations inside other
// DLLs (the CRT itself, Windows) are not seen.
//
// Since SHIP-1 every target links the C runtime statically, so there is no
// malloc import to redirect: the CRT's allocator is compiled into this exe
// and reaches the process heap through kernel32's HeapAlloc / HeapReAlloc,
// which are counted instead. That sees malloc, calloc, realloc and the CRT's
// own internal allocations alike (operator new is counted twice, which only
// the ">= n" checks can notice).
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstring>

namespace {
using MallocFn = void*(__cdecl*)(std::size_t);
using CallocFn = void*(__cdecl*)(std::size_t, std::size_t);
using ReallocFn = void*(__cdecl*)(void*, std::size_t);
using AlignedMallocFn = void*(__cdecl*)(std::size_t, std::size_t);
using AlignedReallocFn = void*(__cdecl*)(void*, std::size_t, std::size_t);

MallocFn realMalloc = nullptr;
CallocFn realCalloc = nullptr;
ReallocFn realRealloc = nullptr;
AlignedMallocFn realAlignedMalloc = nullptr;
AlignedReallocFn realAlignedRealloc = nullptr;
using HeapAllocFn = LPVOID(WINAPI*)(HANDLE, DWORD, SIZE_T);
using HeapReAllocFn = LPVOID(WINAPI*)(HANDLE, DWORD, LPVOID, SIZE_T);
HeapAllocFn realHeapAlloc = nullptr;
HeapReAllocFn realHeapReAlloc = nullptr;

void noteCrtAllocation()
{
    if (khensu::test::allocationGuardArmed) khensu::test::allocationViolations.fetch_add(1);
}
void* __cdecl countingMalloc(std::size_t n)
{
    noteCrtAllocation();
    return realMalloc(n);
}
void* __cdecl countingCalloc(std::size_t count, std::size_t n)
{
    noteCrtAllocation();
    return realCalloc(count, n);
}
void* __cdecl countingRealloc(void* p, std::size_t n)
{
    noteCrtAllocation();
    return realRealloc(p, n);
}
void* __cdecl countingAlignedMalloc(std::size_t n, std::size_t alignment)
{
    noteCrtAllocation();
    return realAlignedMalloc(n, alignment);
}
void* __cdecl countingAlignedRealloc(void* p, std::size_t n, std::size_t alignment)
{
    noteCrtAllocation();
    return realAlignedRealloc(p, n, alignment);
}

LPVOID WINAPI countingHeapAlloc(HANDLE heap, DWORD flags, SIZE_T n)
{
    noteCrtAllocation();
    return realHeapAlloc(heap, flags, n);
}
LPVOID WINAPI countingHeapReAlloc(HANDLE heap, DWORD flags, LPVOID p, SIZE_T n)
{
    noteCrtAllocation();
    return realHeapReAlloc(heap, flags, p, n);
}

/// Points one import-table slot at `replacement`, keeping the original.
template <typename Fn>
void patchSlot(ULONG_PTR* slot, Fn replacement, Fn& original)
{
    DWORD oldProtect = 0;
    if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &oldProtect)) return;
    if (original == nullptr) original = reinterpret_cast<Fn>(*slot);
    *slot = reinterpret_cast<ULONG_PTR>(replacement);
    VirtualProtect(slot, sizeof(*slot), oldProtect, &oldProtect);
}

/// Walks this exe's import descriptors and redirects the malloc family (a
/// DLL C runtime) or the heap calls the static one makes.
/// Returns how many slots were patched (0 = the guard cannot see malloc).
int hookCrtAllocator()
{
    auto* base = reinterpret_cast<BYTE*>(GetModuleHandleW(nullptr));
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (dir.VirtualAddress == 0) return 0;
    int patched = 0;
    for (auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); desc->Name != 0; ++desc) {
        if (desc->OriginalFirstThunk == 0) continue;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->OriginalFirstThunk);
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->FirstThunk);
        for (; names->u1.AddressOfData != 0; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            const char* name = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData)->Name;
            auto* slot = reinterpret_cast<ULONG_PTR*>(&slots->u1.Function);
            if (std::strcmp(name, "malloc") == 0) { patchSlot(slot, &countingMalloc, realMalloc); ++patched; }
            else if (std::strcmp(name, "calloc") == 0) { patchSlot(slot, &countingCalloc, realCalloc); ++patched; }
            else if (std::strcmp(name, "realloc") == 0) { patchSlot(slot, &countingRealloc, realRealloc); ++patched; }
            else if (std::strcmp(name, "_aligned_malloc") == 0) { patchSlot(slot, &countingAlignedMalloc, realAlignedMalloc); ++patched; }
            else if (std::strcmp(name, "_aligned_realloc") == 0) { patchSlot(slot, &countingAlignedRealloc, realAlignedRealloc); ++patched; }
            else if (std::strcmp(name, "HeapAlloc") == 0) { patchSlot(slot, &countingHeapAlloc, realHeapAlloc); ++patched; }
            else if (std::strcmp(name, "HeapReAlloc") == 0) { patchSlot(slot, &countingHeapReAlloc, realHeapReAlloc); ++patched; }
        }
    }
    return patched;
}
} // namespace

namespace khensu::test {
const int crtAllocatorHookedSlots = hookCrtAllocator();
} // namespace khensu::test
#else
namespace khensu::test {
const int crtAllocatorHookedSlots = 0;
} // namespace khensu::test
#endif
