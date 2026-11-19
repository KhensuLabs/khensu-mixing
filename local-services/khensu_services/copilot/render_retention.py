"""Retention sweeps for render leftovers (audit SVC-10 / CPL-19).

Two kinds of render output were never cleaned up:

* ``.quarantine_<name>.wav`` files. A guardrail-rejected render is kept in
  quarantine so the user can still promote it via the expert override, but
  nothing ever deleted one that nobody promoted.
* ``mastering_agent_output/<job_id>/`` directories, one per mastering-agent
  run, each holding every iteration's WAV.

Both are swept by age: anything older than the retention window (default 7
days, ``KHENSU_RENDER_RETENTION_DAYS``) is removed the next time a render
writes into the same place. Recent leftovers stay, so a just-rejected render
can still be overridden and a recent agent run can still be auditioned.
Every sweep is best-effort: a locked or vanished file is skipped, never an
error for the render that triggered the sweep.
"""
from __future__ import annotations

import logging
import os
import shutil
import time

_log = logging.getLogger(__name__)

QUARANTINE_PREFIX = ".quarantine_"


def retention_seconds() -> float:
    """The retention window in seconds (env override, floor of 1 hour)."""
    try:
        days = float(os.environ.get("KHENSU_RENDER_RETENTION_DAYS", "7"))
    except ValueError:
        days = 7.0
    return max(3600.0, days * 86400.0)


def _older_than(path: str, cutoff: float) -> bool:
    try:
        return os.path.getmtime(path) < cutoff
    except OSError:
        return False


def sweep_stale_quarantine(directory: str, max_age_sec: float | None = None,
                           now: float | None = None) -> int:
    """Delete ``.quarantine_*`` files in ``directory`` older than the window.
    Returns how many were removed."""
    if not directory or not os.path.isdir(directory):
        return 0
    cutoff = (time.time() if now is None else now) - (retention_seconds() if max_age_sec is None else max_age_sec)
    removed = 0
    try:
        names = os.listdir(directory)
    except OSError:
        return 0
    for name in names:
        if not name.startswith(QUARANTINE_PREFIX):
            continue
        path = os.path.join(directory, name)
        if os.path.isfile(path) and _older_than(path, cutoff):
            try:
                os.remove(path)
                removed += 1
            except OSError:
                _log.debug("Could not remove stale quarantine %s", path, exc_info=True)
    return removed


def sweep_stale_job_dirs(root: str, keep: set[str] | None = None,
                         max_age_sec: float | None = None, now: float | None = None) -> int:
    """Delete per-job subdirectories of ``root`` whose newest file is older
    than the window, except the names in ``keep``. Returns how many went."""
    if not root or not os.path.isdir(root):
        return 0
    cutoff = (time.time() if now is None else now) - (retention_seconds() if max_age_sec is None else max_age_sec)
    keep = keep or set()
    removed = 0
    try:
        entries = os.listdir(root)
    except OSError:
        return 0
    for name in entries:
        path = os.path.join(root, name)
        if name in keep or not os.path.isdir(path):
            continue
        newest = 0.0
        try:
            newest = os.path.getmtime(path)
            for dirpath, _dirs, files in os.walk(path):
                for f in files:
                    newest = max(newest, os.path.getmtime(os.path.join(dirpath, f)))
        except OSError:
            continue
        if newest < cutoff:
            shutil.rmtree(path, ignore_errors=True)
            if not os.path.exists(path):
                removed += 1
    return removed
