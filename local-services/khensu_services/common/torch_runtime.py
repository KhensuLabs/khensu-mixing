"""torch's CPU thread cap, applied when torch is first used (PERF-4).

Left unbounded, torch uses every core for a single inference call; on a
many-core machine that starves uvicorn's event-loop thread for the whole run
(a healthy, sub-10 ms /health went unresponsive for the length of a CPU Demucs
separation, then recovered). The cap reserves 2 cores for the event loop and
the executors, and stays at 4 or below so a small machine isn't starved.

``server.py`` used to import torch at start-up just to set this, which cost
~1.4 s (about half the service's import time) and hundreds of MB resident on
every launch, even for users without the stems profile. Every module that
runs torch inference calls :func:`ensure_threads` right after importing it
instead. ``OMP_NUM_THREADS`` would also cap numpy/scipy's BLAS, which is why
the cap is set on torch alone.
"""
from __future__ import annotations

import os
import threading

_lock = threading.Lock()
_applied = False


def thread_cap() -> int:
    """The intra-op thread count torch is limited to."""
    return max(1, min(4, (os.cpu_count() or 4) - 2))


def ensure_threads() -> bool:
    """Caps torch's intra-op threads once per process. False without torch."""
    global _applied
    if _applied:
        return True
    with _lock:
        if _applied:
            return True
        try:
            import torch
        except ImportError:
            return False
        torch.set_num_threads(thread_cap())
        _applied = True
        return True
