"""Local-only crash capture for the Python service.

``faulthandler`` dumps every thread's Python stack when the interpreter dies
on a fatal signal (access violation in a native extension, abort, stack
overflow). The desktop app writes its minidumps to the same
``%LOCALAPPDATA%/KhensuMixing/crashes`` directory (``<config dir>/crashes``
off Windows), so one folder holds everything a bug
report needs. Nothing is uploaded, ever.
"""
from __future__ import annotations

import faulthandler
import logging
import logging.handlers
import os
import sys
import threading
import traceback
from datetime import datetime
from pathlib import Path
from typing import IO, Any

from khensu_services.common.config import config_dir

#: Past this size the log is restarted rather than appended to.
MAX_LOG_BYTES = 1_000_000

_log_file: IO[str] | None = None
_error_handler: logging.Handler | None = None
_hooks_installed = False


def diagnostics_root() -> Path:
    """Machine-local diagnostics root (CE-ENG-4): ``%LOCALAPPDATA%/KhensuMixing``
    on Windows - crash dumps and traces are large and machine-specific, so
    they must not roam with the profile the way settings.json does. Elsewhere
    it is the config dir. ``KHENSU_DIAGNOSTICS_DIR`` overrides (tests)."""
    override = os.environ.get("KHENSU_DIAGNOSTICS_DIR", "").strip()
    if override:
        return Path(override)
    if sys.platform == "win32":
        base = os.environ.get("LOCALAPPDATA")
        if base:
            return Path(base) / "KhensuMixing"
    return config_dir()


def crashes_dir() -> Path:
    """Where the desktop's and the plugin's minidumps, the service's
    faulthandler log and its crash log all go (one folder per bug report)."""
    return diagnostics_root() / "crashes"


def traces_dir() -> Path:
    return diagnostics_root() / "traces"


def _write_crash_record(kind: str, exc_type: Any, exc: Any, tb: Any, thread_name: str = "") -> None:
    """Append one unhandled-exception record to ``service-crash.log``.
    Never raises: this runs inside an excepthook."""
    try:
        d = crashes_dir()
        d.mkdir(parents=True, exist_ok=True)
        path = d / "service-crash.log"
        mode = "w" if path.exists() and path.stat().st_size > MAX_LOG_BYTES else "a"
        from khensu_services.version import SERVICE_VERSION
        text = "".join(traceback.format_exception(exc_type, exc, tb))
        with open(path, mode, encoding="utf-8") as f:
            f.write(f"=== {datetime.now().isoformat(timespec='seconds')} {kind}"
                    f"{' [' + thread_name + ']' if thread_name else ''}"
                    f" service {SERVICE_VERSION} pid {os.getpid()}\n{text}\n")
    except Exception:
        pass


def install_crash_log(directory: Path | None = None) -> Path | None:
    """Unhandled-exception capture for the service (CE-ENG-4).

    * ``sys.excepthook`` / ``threading.excepthook``: an exception that kills
      the main thread or a worker thread is appended to
      ``service-crash.log`` (then the previous hook runs as before).
    * A size-bounded file handler on the ``khensu_services.error`` logger, so
      every route's unhandled exception (logged by the global handler with
      its correlation id) is kept in ``service-errors.log``.

    Idempotent; never raises. Returns the error-log path, or None."""
    global _hooks_installed, _error_handler
    if not _hooks_installed:
        prev_sys = sys.excepthook
        prev_thread = threading.excepthook

        def _sys_hook(exc_type, exc, tb):
            if not issubclass(exc_type, KeyboardInterrupt):
                _write_crash_record("unhandled exception", exc_type, exc, tb)
            prev_sys(exc_type, exc, tb)

        def _thread_hook(args):
            if args.exc_type is not SystemExit:
                name = args.thread.name if args.thread is not None else ""
                _write_crash_record("unhandled thread exception", args.exc_type,
                                    args.exc_value, args.exc_traceback, name)
            prev_thread(args)

        sys.excepthook = _sys_hook
        threading.excepthook = _thread_hook
        _hooks_installed = True
    try:
        d = directory if directory is not None else crashes_dir()
        d.mkdir(parents=True, exist_ok=True)
        path = d / "service-errors.log"
        logger = logging.getLogger("khensu_services.error")
        if _error_handler is not None:
            logger.removeHandler(_error_handler)
            _error_handler.close()
        handler = logging.handlers.RotatingFileHandler(
            path, maxBytes=MAX_LOG_BYTES, backupCount=1, encoding="utf-8", delay=True)
        handler.setFormatter(logging.Formatter("%(asctime)s %(levelname)s %(name)s: %(message)s"))
        logger.addHandler(handler)
        _error_handler = handler
        return path
    except OSError:
        return None


def enable_faulthandler(directory: Path | None = None) -> Path | None:
    """Route faulthandler output to ``service-faulthandler.log``. Never raises.

    The file is appended to (a crash loop keeps every trace) but truncated
    once it passes ``MAX_LOG_BYTES`` so it cannot grow without bound. Returns
    the log path, or None if it could not be opened (faulthandler then stays
    on stderr, its default).
    """
    global _log_file
    try:
        target_dir = directory if directory is not None else crashes_dir()
        target_dir.mkdir(parents=True, exist_ok=True)
        path = target_dir / "service-faulthandler.log"
        mode = "w" if path.exists() and path.stat().st_size > MAX_LOG_BYTES else "a"
        # Kept open for the life of the process: faulthandler writes to the
        # raw fd at crash time, when nothing can be opened any more.
        handle = open(path, mode, encoding="utf-8")  # noqa: SIM115
        faulthandler.enable(file=handle, all_threads=True)
        if _log_file is not None and _log_file is not handle:
            _log_file.close()
        _log_file = handle
        return path
    except OSError:
        try:
            faulthandler.enable(all_threads=True)
        except (OSError, RuntimeError, ValueError):
            pass
        return None
