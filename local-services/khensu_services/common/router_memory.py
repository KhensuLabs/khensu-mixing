"""GET /diagnostics/memory (PERF-7): what the service's caches and pools hold.

The analysis caches each had their own small budget (``BoundedCache``,
~8 MB) but nothing bounded their sum, and nothing showed where memory went.
Every ``BoundedCache`` now registers itself (``common/lru``) under one
process-wide byte budget, and this route reports each cache, the decoded-audio
cache, the thread pools and the process's resident memory.
"""
from __future__ import annotations

import ctypes
import os
import sys

from fastapi import APIRouter

from khensu_services.common.api_models import ResponseModel
from khensu_services.common.executors import io_route

router = APIRouter(tags=["diagnostics"])


class CacheInfo(ResponseModel):
    name: str
    entries: int
    bytes: int


class PoolInfo(ResponseModel):
    name: str
    workers: int


class MemoryGateInfo(ResponseModel):
    """common/memory_gate: the jobs' memory budget and how often it held one back."""
    enabled: bool
    budget_bytes: int
    in_use_bytes: int
    waits: int
    overcommits: int


class MemoryResponse(ResponseModel):
    rss_bytes: int | None = None
    result_cache_bytes: int
    result_cache_budget_bytes: int
    result_caches: list[CacheInfo] = []
    decoded_audio_bytes: int
    decoded_audio_budget_bytes: int
    decoded_audio_entries: int
    pools: list[PoolInfo] = []
    cpu_count: int
    memory_gate: MemoryGateInfo | None = None


def process_rss_bytes() -> int | None:
    """Resident set size of this process, without psutil (not a dependency)."""
    try:
        if sys.platform == "win32":
            from ctypes import wintypes

            class _Counters(ctypes.Structure):
                _fields_ = [("cb", wintypes.DWORD), ("PageFaultCount", wintypes.DWORD),
                            ("PeakWorkingSetSize", ctypes.c_size_t), ("WorkingSetSize", ctypes.c_size_t),
                            ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
                            ("QuotaPagedPoolUsage", ctypes.c_size_t),
                            ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
                            ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
                            ("PagefileUsage", ctypes.c_size_t), ("PeakPagefileUsage", ctypes.c_size_t)]

            counters = _Counters()
            counters.cb = ctypes.sizeof(_Counters)
            k32 = ctypes.WinDLL("kernel32", use_last_error=True)
            psapi = ctypes.WinDLL("psapi", use_last_error=True)
            k32.GetCurrentProcess.restype = wintypes.HANDLE
            psapi.GetProcessMemoryInfo.argtypes = [wintypes.HANDLE, ctypes.POINTER(_Counters), wintypes.DWORD]
            if psapi.GetProcessMemoryInfo(k32.GetCurrentProcess(), ctypes.byref(counters), counters.cb):
                return int(counters.WorkingSetSize)
            return None
        if sys.platform == "darwin":
            return _macos_rss_bytes()
        with open("/proc/self/statm", encoding="ascii") as f:
            return int(f.read().split()[1]) * os.sysconf("SC_PAGE_SIZE")
    except (OSError, ValueError, AttributeError):
        return None


def _macos_rss_bytes() -> int | None:
    """MAC-1: macOS has no /proc - the resident size from the Mach kernel's
    task_info(MACH_TASK_BASIC_INFO), the figure Activity Monitor shows as
    "Real Memory"."""
    class _TimeValue(ctypes.Structure):
        _fields_ = [("seconds", ctypes.c_int), ("microseconds", ctypes.c_int)]

    class _MachTaskBasicInfo(ctypes.Structure):
        _fields_ = [("virtual_size", ctypes.c_uint64), ("resident_size", ctypes.c_uint64),
                    ("resident_size_max", ctypes.c_uint64), ("user_time", _TimeValue),
                    ("system_time", _TimeValue), ("policy", ctypes.c_int), ("suspend_count", ctypes.c_int)]

    libc = ctypes.CDLL("/usr/lib/libSystem.B.dylib")
    task = ctypes.c_uint.in_dll(libc, "mach_task_self_")
    info = _MachTaskBasicInfo()
    count = ctypes.c_uint(ctypes.sizeof(info) // ctypes.sizeof(ctypes.c_uint))
    MACH_TASK_BASIC_INFO = 20
    if libc.task_info(task, MACH_TASK_BASIC_INFO, ctypes.byref(info), ctypes.byref(count)) == 0:
        return int(info.resident_size)
    import resource
    return int(resource.getrusage(resource.RUSAGE_SELF).ru_maxrss)  # peak, bytes on macOS


def memory_report() -> dict:
    from khensu_services.analysis.context import decoded_audio_cache as _DECODED_CACHE
    from khensu_services.common import executors, lru, memory_gate

    caches = lru.cache_stats()
    return {
        "rss_bytes": process_rss_bytes(),
        "result_cache_bytes": sum(c["bytes"] for c in caches),
        "result_cache_budget_bytes": lru.global_budget_bytes(),
        "result_caches": sorted(caches, key=lambda c: -c["bytes"]),
        "decoded_audio_bytes": _DECODED_CACHE.total_bytes,
        "decoded_audio_budget_bytes": _DECODED_CACHE.max_bytes,
        "decoded_audio_entries": len(_DECODED_CACHE),
        "pools": [{"name": n, "workers": w} for n, w in executors.pool_sizes().items()],
        "cpu_count": os.cpu_count() or 1,
        "memory_gate": memory_gate.gate.stats(),
    }


@router.get("/diagnostics/memory", response_model=MemoryResponse, response_model_exclude_unset=True)
@io_route
def diagnostics_memory() -> dict:
    """Where the service's memory is: each result cache, the decoded-audio
    cache, the pools and resident memory (PERF-7)."""
    return memory_report()
