"""Graceful process exit for the local service (audit SVC-02).

``POST /shutdown`` used to ``os.kill(getpid(), SIGTERM)``. On Windows that is
``TerminateProcess``: the lifespan ``finally`` never ran, so in-flight writes
(quarantine renders, the Demucs ``.th.part`` download, album WAV/DDP exports,
reports) were cut off mid-file. Now:

1. ``launch_service.py`` builds the ``uvicorn.Server`` itself and registers it
   here (``set_server``).
2. ``request_exit()`` sets ``server.should_exit`` - Uvicorn's own graceful
   path on every platform: stop accepting, let open requests finish (bounded
   by ``timeout_graceful_shutdown``), run the lifespan shutdown, which stops
   every executor with ``cancel_futures=True``.
   Without a registered server (``uvicorn khensu_services.server:app`` run by
   hand) it falls back to ``signal.raise_signal(SIGINT)``, which Uvicorn's
   installed handler turns into the same ``should_exit``.
3. After ``server.run()`` returns, ``wait_for_workers()`` gives in-flight
   worker threads a bounded grace period (their jobs are already marked
   ``interrupted``, so cooperative workers stop at their next check) before
   the launcher exits the process.
4. ``start_exit_watchdog()`` bounds the whole exit: once ``should_exit`` is
   set, the process is gone within ``HARD_EXIT_DEADLINE_SEC`` however the
   graceful path ends. A real run (2026-11-14) left a service alive with its
   listening socket closed - Uvicorn had begun shutting down and never
   finished - and the desktop, seeing the process alive, never replaced it.
   Before force-exiting, the watchdog writes every thread's stack to
   ``<config dir>/logs/service-exit-hang-<time>.txt`` so the hang can be
   traced.
"""
from __future__ import annotations

import logging
import signal
import threading
import time
from typing import Any

_log = logging.getLogger(__name__)

# Seconds Uvicorn waits for open requests (an SSE chat stream, a long sync
# render) before closing them on exit.
GRACEFUL_REQUEST_TIMEOUT_SEC = 3
# Seconds the launcher waits for executor worker threads after Uvicorn exits.
# The desktop allows 3.5 s for the process to exit before it kills the job.
WORKER_GRACE_SEC = 2.5
# Seconds from should_exit to a forced exit, whatever the graceful path is
# waiting on. Comfortably above the 3 s request + 2.5 s worker graces.
HARD_EXIT_DEADLINE_SEC = 15.0

_server: Any = None
_server_lock = threading.Lock()


def set_server(server: Any) -> None:
    """Register the running ``uvicorn.Server`` (launcher only)."""
    global _server
    with _server_lock:
        _server = server


def get_server() -> Any:
    with _server_lock:
        return _server


def request_exit() -> str:
    """Ask Uvicorn to exit gracefully. Returns the mechanism used
    (``"server"`` or ``"signal"``), for logging/tests."""
    server = get_server()
    if server is not None:
        server.should_exit = True
        return "server"
    # No handle: a hand-started uvicorn. SIGINT reaches Uvicorn's handler
    # (never TerminateProcess, unlike os.kill(SIGTERM) on Windows).
    signal.raise_signal(signal.SIGINT)
    return "signal"


def wait_for_workers(grace_sec: float = WORKER_GRACE_SEC) -> bool:
    """Wait up to ``grace_sec`` for the executor worker threads to finish.
    True when none is still alive. Never raises."""
    from khensu_services.common.executors import ALL_EXECUTORS

    deadline = time.monotonic() + max(0.0, grace_sec)
    threads = []
    for ex in ALL_EXECUTORS:
        threads.extend(list(getattr(ex, "_threads", ()) or ()))
    for t in threads:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            break
        try:
            t.join(remaining)
        except RuntimeError:
            pass
    alive = [t.name for t in threads if t.is_alive()]
    if alive:
        _log.warning("Service exit: %d worker thread(s) still busy after %.1fs: %s",
                     len(alive), grace_sec, ", ".join(alive[:8]))
    return not alive


def hang_report_path(now: float | None = None) -> Any:
    """Where the watchdog writes thread stacks before a forced exit."""
    from khensu_services.common.config import config_dir

    stamp = time.strftime("%Y%m%d-%H%M%S", time.localtime(now if now is not None else time.time()))
    return config_dir() / "logs" / f"service-exit-hang-{stamp}.txt"


def _write_hang_report(deadline_sec: float) -> None:
    """Dump every thread's stack to the hang report. Never raises."""
    import faulthandler

    try:
        path = hang_report_path()
        path.parent.mkdir(parents=True, exist_ok=True)
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(f"Service exit did not finish within {deadline_sec:.0f} s of should_exit; "
                     "forcing exit. Thread stacks:\n\n")
            fh.flush()
            faulthandler.dump_traceback(file=fh, all_threads=True)
        _log.error("Service exit hung for %.0f s; stacks written to %s", deadline_sec, path)
    except Exception:  # noqa: BLE001 - the forced exit must still happen
        _log.exception("Service exit hung; could not write the hang report")


def start_exit_watchdog(server: Any, deadline_sec: float = HARD_EXIT_DEADLINE_SEC,
                        poll_sec: float = 0.25, exit_fn: Any = None) -> threading.Thread:
    """Force the process out ``deadline_sec`` after ``server.should_exit``.

    A daemon thread, so it never keeps a finished process alive. ``exit_fn``
    (default ``os._exit``) is injectable for tests; it is called with 3.
    """
    import os

    do_exit = exit_fn if exit_fn is not None else os._exit

    def _watch() -> None:
        while not getattr(server, "should_exit", False):
            time.sleep(poll_sec)
        time.sleep(max(0.0, deadline_sec))
        _write_hang_report(deadline_sec)
        do_exit(3)

    t = threading.Thread(target=_watch, name="khensu-exit-watchdog", daemon=True)
    t.start()
    return t


#: The desktop app's process id (MAC-1). macOS and Linux have no job object
#: that kills the service with the app (Windows: ServiceProcessJob), so a
#: crashed app left the service on its port. The app sets this before it
#: spawns the service; the service leaves when that process is gone.
PARENT_PID_ENV = "KHENSU_PARENT_PID"


def start_parent_watch(poll_sec: float = 1.0, exit_fn: Any = None,
                       getppid: Any = None, environ: Any = None) -> threading.Thread | None:
    """Request a graceful exit once the app that started the service is gone
    (it exited, crashed or was killed: the service is re-parented). POSIX
    only - on Windows the job object does this, and ``os.kill(pid, 0)`` there
    would terminate the process. None when not started by the app."""
    import os
    import sys

    env = environ if environ is not None else os.environ
    raw = str(env.get(PARENT_PID_ENV, "")).strip()
    if sys.platform == "win32" or not raw.isdigit():
        return None
    parent = int(raw)
    ppid = getppid or os.getppid
    leave = exit_fn or request_exit

    def _alive(pid: int) -> bool:
        try:
            os.kill(pid, 0)
            return True
        except ProcessLookupError:
            return False
        except PermissionError:
            return True  # exists, owned by someone else

    def _watch() -> None:
        while ppid() == parent and _alive(parent):
            time.sleep(poll_sec)
        leave()

    t = threading.Thread(target=_watch, name="khensu-parent-watch", daemon=True)
    t.start()
    return t
