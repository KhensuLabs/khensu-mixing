"""The one list of background-job lifecycle states.

Python job stores, the desktop app and the plugin all poll these strings.
They used to be typed separately in each codebase, which is how the desktop
waited for ``"done"`` while the store said ``"completed"`` and how
``"interrupted"`` was never treated as terminal. ``scripts/gen_cpp_api.py``
generates ``khensu::api::JobStatus`` (``shared-core/include/khensu/ipc/
generated/ApiJobStatus.h``) from this enum, so the C++ side cannot drift.
"""
from __future__ import annotations

from enum import Enum


class JobStatus(str, Enum):
    PENDING = "pending"          # accepted, not started
    QUEUED = "queued"            # waiting for an executor slot
    RUNNING = "running"
    CANCELLING = "cancelling"    # cancel requested, worker still unwinding
    COMPLETED = "completed"
    FAILED = "failed"
    CANCELLED = "cancelled"
    INTERRUPTED = "interrupted"  # the service restarted underneath the job


TERMINAL_STATES: frozenset[JobStatus] = frozenset(
    {JobStatus.COMPLETED, JobStatus.FAILED, JobStatus.CANCELLED, JobStatus.INTERRUPTED}
)
CANCELLABLE_STATES: frozenset[JobStatus] = frozenset(
    {JobStatus.PENDING, JobStatus.QUEUED, JobStatus.RUNNING}
)


def is_terminal(status: str) -> bool:
    return status in {s.value for s in TERMINAL_STATES}
