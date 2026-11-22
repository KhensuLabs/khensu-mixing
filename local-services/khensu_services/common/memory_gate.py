"""Admission by memory for analysis jobs (2026-11-18).

One request's peak grows with the track: about 10x its decoded audio
(927 MB for a 4:20 stereo track, 2.4 GB for 12:30, the longest the decode
budget allows). The pools limit how many jobs run, not how much memory they
use, so with a long file the Dashboard's follow-up scans could ask for far
more than the machine has.

So a job that loads audio is charged ``WEIGHT_PER_DECODED_BYTE`` x the
decoded array's size against one process-wide budget (installed RAM minus
3 GB, ``KHENSU_MEMORY_BUDGET_MB`` overrides) and waits while that budget is
spent. Its charge is released when the job ends (the pools' submit wrapper,
:class:`GatedThreadPoolExecutor`).

What keeps this from deadlocking:

* only pool threads are gated (a test or a synchronous caller never waits);
* a job that already holds a charge is never made to wait for more (a job
  comparing two files charges both but blocks only before its first);
* a single job larger than the budget is admitted once nothing else runs;
* a wait gives up after ``MAX_WAIT_S`` and proceeds, with a log line.

``KHENSU_MEMORY_GATE=0`` turns it off.
"""
from __future__ import annotations

import logging
import os
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from typing import Any

_log = logging.getLogger(__name__)

WEIGHT_PER_DECODED_BYTE = 10.0
MAX_WAIT_S = 120.0
_RESERVED_BYTES = 3 * 2**30
_MIN_BUDGET_BYTES = 1 * 2**30


def physical_memory_bytes() -> int | None:
    """Installed RAM in bytes, or None when it cannot be read (no psutil in
    the service's environment: the platform call directly)."""
    try:
        if os.name == "nt":
            import ctypes

            class _MemStatus(ctypes.Structure):
                _fields_ = [("dwLength", ctypes.c_ulong), ("dwMemoryLoad", ctypes.c_ulong),
                            ("ullTotalPhys", ctypes.c_ulonglong), ("ullAvailPhys", ctypes.c_ulonglong),
                            ("ullTotalPageFile", ctypes.c_ulonglong), ("ullAvailPageFile", ctypes.c_ulonglong),
                            ("ullTotalVirtual", ctypes.c_ulonglong), ("ullAvailVirtual", ctypes.c_ulonglong),
                            ("ullAvailExtendedVirtual", ctypes.c_ulonglong)]

            status = _MemStatus()
            status.dwLength = ctypes.sizeof(_MemStatus)
            if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(status)):
                return int(status.ullTotalPhys)
            return None
        if sys.platform == "darwin":
            # MAC-1: macOS has no SC_PHYS_PAGES; sysctl hw.memsize is the RAM.
            import ctypes
            libc = ctypes.CDLL("/usr/lib/libSystem.B.dylib")
            size = ctypes.c_uint64(0)
            length = ctypes.c_size_t(ctypes.sizeof(size))
            if libc.sysctlbyname(b"hw.memsize", ctypes.byref(size), ctypes.byref(length), None, 0) == 0:
                return int(size.value)
            return None
        return int(os.sysconf("SC_PAGE_SIZE")) * int(os.sysconf("SC_PHYS_PAGES"))
    except (AttributeError, OSError, ValueError):
        return None


def _default_budget() -> int:
    override = os.environ.get("KHENSU_MEMORY_BUDGET_MB", "").strip()
    if override:
        try:
            return max(64 * 2**20, int(float(override) * 2**20))
        except ValueError:
            pass
    ram = physical_memory_bytes()
    if not ram:
        return 8 * 2**30
    return max(_MIN_BUDGET_BYTES, ram - _RESERVED_BYTES)


class MemoryGate:
    """A byte budget shared by the pool threads (see the module docstring)."""

    def __init__(self, budget_bytes: int | None = None, enabled: bool | None = None) -> None:
        self._budget = int(budget_bytes) if budget_bytes is not None else _default_budget()
        if enabled is None:
            enabled = os.environ.get("KHENSU_MEMORY_GATE", "1").strip() != "0"
        self.enabled = enabled
        self._used = 0
        self._cond = threading.Condition()
        self._local = threading.local()
        self.waits = 0
        self.overcommits = 0

    # -- per-job scope (entered by the pool wrapper) --------------------------
    def begin_job(self) -> None:
        self._local.held = {}
        self._local.in_job = True

    def end_job(self) -> None:
        held = getattr(self._local, "held", None) or {}
        self._local.held = {}
        self._local.in_job = False
        total = sum(held.values())
        if total:
            with self._cond:
                self._used -= total
                self._cond.notify_all()

    # -- charging ---------------------------------------------------------------
    def charge(self, key: Any, decoded_bytes: int) -> None:
        """Charge this job for audio ``key`` (once per key per job)."""
        if not self.enabled or not getattr(self._local, "in_job", False):
            return
        held: dict = self._local.held
        cost = int(decoded_bytes * WEIGHT_PER_DECODED_BYTE)
        extra = cost - held.get(key, 0)
        if extra <= 0:
            return
        with self._cond:
            if held:
                # Already holding: never wait (no hold-and-wait, no deadlock).
                if self._used + extra > self._budget:
                    self.overcommits += 1
            else:
                deadline = time.monotonic() + MAX_WAIT_S
                waited = False
                # A job bigger than the whole budget goes in when nothing runs.
                while self._used > 0 and self._used + extra > self._budget:
                    left = deadline - time.monotonic()
                    if left <= 0:
                        self.overcommits += 1
                        _log.warning("memory gate: proceeding after %.0f s (%.0f MB in use, %.0f MB asked)",
                                     MAX_WAIT_S, self._used / 1e6, extra / 1e6)
                        break
                    waited = True
                    self._cond.wait(min(left, 1.0))
                if waited:
                    self.waits += 1
            self._used += extra
        held[key] = cost

    def stats(self) -> dict[str, Any]:
        with self._cond:
            return {"enabled": self.enabled, "budget_bytes": self._budget, "in_use_bytes": self._used,
                    "waits": self.waits, "overcommits": self.overcommits}


gate = MemoryGate()


class GatedThreadPoolExecutor(ThreadPoolExecutor):
    """A ThreadPoolExecutor whose jobs release their memory charge on exit."""

    def submit(self, fn, /, *args, **kwargs):  # type: ignore[override]
        def run(*a: Any, **k: Any) -> Any:
            gate.begin_job()
            try:
                return fn(*a, **k)
            finally:
                gate.end_job()

        return super().submit(run, *args, **kwargs)
