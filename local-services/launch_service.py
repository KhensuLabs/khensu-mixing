"""Entry-point called by the desktop app to start the local analysis service.

On first run (or if the venv is missing), this script bootstraps a virtual
environment and installs the reviewed base runtime automatically — no manual
pip install required. Subsequent launches detect the existing venv and start
instantly.

Bootstrap sequence:
  1. Detect whether the venv exists: .venv next to this file in a checkout;
     in an installed app (a macOS .app bundle, or a folder this user cannot
     write) the per-user ~/Library/KhensuMixing/service-venv
     (%LOCALAPPDATA%/KhensuMixing/service-venv, ~/.local/share/...), so
     setup never writes into the signed bundle (MAC-1).
  2. If not, create it via `python -m venv .venv`, install the hash-verified
     base runtime, then install the local package without dependency resolution
     (this only happens once; typically takes 1–3 minutes on first run).
  3. Re-exec from the venv Python so all packages are available.
  4. Start uvicorn.
"""
import os
import sys
import subprocess
from pathlib import Path
from typing import MutableMapping

HERE = Path(__file__).parent.resolve()

#: The Pythons the locked runtime has wheels for on every shipped platform
#: (scripts/check_platform_wheels.py). Intel Macs stop at 3.13: numba
#: publishes no Intel build for 3.14 (MAC-1).
SUPPORTED_PYTHONS = ((3, 11), (3, 12), (3, 13))


def python_supported(version_info=None) -> bool:
    v = version_info or sys.version_info
    return (v[0], v[1]) in SUPPORTED_PYTHONS and getattr(v, "releaselevel", "final") == "final"


def installed_bundle(here: Path = HERE) -> bool:
    """True when this file ships inside an installed app: a macOS .app bundle
    (writing there breaks its code signature) or a folder this user cannot
    write (Program Files, /Applications)."""
    if ".app/Contents/" in here.as_posix():
        return True
    return not os.access(here, os.W_OK)


def user_venv_dir() -> Path:
    if sys.platform == "darwin":
        base = Path.home() / "Library" / "KhensuMixing"
    elif sys.platform == "win32":
        base = Path(os.environ.get("LOCALAPPDATA") or Path.home()) / "KhensuMixing"
    else:
        base = Path(os.environ.get("XDG_DATA_HOME") or Path.home() / ".local" / "share") / "KhensuMixing"
    return base / "service-venv"


def venv_python(venv: Path) -> Path:
    if sys.platform == "win32":
        return venv / "Scripts" / "python.exe"
    py = venv / "bin" / "python3"
    return py if py.exists() else venv / "bin" / "python"


VENV_DIR = user_venv_dir() if installed_bundle() else HERE / ".venv"
#: What the venv was built from: a venv made by another Python or from an
#: older lock export is rebuilt.
STAMP_NAME = "khensu-venv-stamp.txt"


def venv_stamp() -> str:
    import hashlib
    req = HERE / "requirements-runtime-hashed.txt"
    digest = hashlib.sha256(req.read_bytes()).hexdigest()[:16] if req.exists() else "none"
    return f"python {sys.version_info[0]}.{sys.version_info[1]} {sys.platform} {digest}"


def _set_offline(env: MutableMapping[str, str]) -> None:
    env["HF_HUB_OFFLINE"] = "1"
    env["TRANSFORMERS_OFFLINE"] = "1"
    env["HF_HUB_DISABLE_TELEMETRY"] = "1"


def apply_offline_env(env: MutableMapping[str, str], settings: dict, *,
                      corrupt: bool = False) -> None:
    """Set HF/transformers offline env vars when downloads/hub are disabled.

    These libraries read the vars at import time, so this runs before the app
    process starts; changing the toggle needs a service restart to take effect.

    ``corrupt`` (a settings.json that exists but can't be parsed) fails
    closed (SVC-15): the file was read as ``{}``, i.e. "everything on", which
    left the HF libraries online even though ``common.online`` treats the same
    file as "everything off".
    """
    if corrupt or not isinstance(settings, dict):
        _set_offline(env)
        return
    online = settings.get("online", {})
    if not isinstance(online, dict):
        _set_offline(env)
        return
    enabled = bool(online.get("enabled", True))
    downloads = enabled and bool(online.get("model_downloads", True))
    hub = enabled and bool(online.get("hf_hub", True))
    if not (downloads and hub):
        _set_offline(env)


def bootstrap_online_allowed() -> bool:
    """Whether first-run bootstrap may download packages (AUDIT-077).

    Reads settings.json through ``common.online`` itself (stdlib-only, so it
    imports fine before the venv exists) so the master online switch means the
    same thing here as in the service: off, or a settings file that exists but
    can't be parsed (fail closed), means no pip download.
    """
    if str(HERE) not in sys.path:
        sys.path.insert(0, str(HERE))
    try:
        from khensu_services.common.online import load_settings, settings_corrupt
        if settings_corrupt():
            return False
        online = load_settings().get("online", {})
        return isinstance(online, dict) and online.get("enabled", True) is not False
    except Exception:
        return False


OFFLINE_BOOTSTRAP_MESSAGE = (
    "[khensu-service] First run needs to download the analysis service's Python "
    "packages, but online features are switched off in Settings. Nothing was "
    "downloaded. Turn on 'Online features' in Settings and restart Khensu, or "
    "install the service manually (see README: 'Python service').")


def bootstrap_venv(run=subprocess.run) -> bool:
    """Create .venv and install the hash-verified base runtime. Returns False
    (without creating anything, so the next launch retries cleanly) when the
    master online switch forbids the download."""
    if not bootstrap_online_allowed():
        print(OFFLINE_BOOTSTRAP_MESSAGE, file=sys.stderr, flush=True)
        return False
    if not python_supported():
        print(f"[khensu-service] Python {sys.version.split()[0]} is not supported: install Python 3.11, "
              f"3.12 or 3.13 (python.org).", file=sys.stderr, flush=True)
        return False
    print(f"[khensu-service] First run: creating virtual environment in {VENV_DIR}...", flush=True)
    VENV_DIR.parent.mkdir(parents=True, exist_ok=True)
    run([sys.executable, "-m", "venv", "--clear", str(VENV_DIR)], check=True)
    print("[khensu-service] Installing dependencies (one-time setup)...", flush=True)
    py = str(venv_python(VENV_DIR))
    run(
        [py, "-m", "pip", "install", "--quiet", "--require-hashes", "-r",
         str(HERE / "requirements-runtime-hashed.txt")],
        check=True,
    )
    if not installed_bundle():
        # A checkout: the package itself, editable. An installed app runs the
        # service from its own folder (sys.path), and never writes into it.
        run([py, "-m", "pip", "install", "--quiet", "--no-deps", "-e", str(HERE)], check=True)
    pip = py
    # MMC Locate's port backend (the [midi] extra), best-effort: python-rtmidi
    # has no wheel for every Python, and the service runs without it (MMC off).
    midi = run([pip, "-m", "pip", "install", "--quiet", "--require-hashes", "-r",
                str(HERE / "requirements-midi-hashed.txt")], check=False)
    if getattr(midi, "returncode", 0) != 0:
        print("[khensu-service] MIDI (MMC Locate) support could not be installed; continuing without it.",
              flush=True)
    VENV_DIR.mkdir(parents=True, exist_ok=True)
    (VENV_DIR / STAMP_NAME).write_text(venv_stamp(), encoding="utf-8")
    print("[khensu-service] Setup complete.", flush=True)
    return True


def venv_current() -> bool:
    """The venv exists and was built by this Python from this lock export."""
    py = venv_python(VENV_DIR)
    stamp = VENV_DIR / STAMP_NAME
    if not py.exists():
        return False
    if not stamp.exists():
        return not installed_bundle()  # a developer's own .venv carries no stamp
    return stamp.read_text(encoding="utf-8").strip() == venv_stamp()


# ── Installed-runtime / venv detection ────────────────────────────────────────
# The installer stages a hash-verified embeddable runtime here. It takes
# priority over developer venvs and PATH so a shipped app never resolves its
# dependencies from an uncontrolled machine environment.
RUNTIME_PYTHON = HERE / "runtime" / ("python.exe" if sys.platform == "win32" else "bin/python3")
VENV_PYTHON = venv_python(VENV_DIR)

def run_service(host: str = "127.0.0.1", port: int = 47239) -> None:
    """Run Uvicorn with a server handle the app can stop gracefully (SVC-02).

    ``POST /shutdown`` sets ``should_exit`` on this server through
    ``common.lifecycle`` instead of ``os.kill(SIGTERM)`` (TerminateProcess on
    Windows, which skipped the lifespan shutdown). After Uvicorn returns, the
    executor workers get a bounded grace period, then the process exits
    without waiting on a worker that ignores its ``interrupted`` job state.
    """
    import uvicorn
    from khensu_services.common import lifecycle

    config = uvicorn.Config(
        "khensu_services.server:app",
        host=host,
        port=port,
        access_log=False,
        log_level="warning",
        timeout_graceful_shutdown=lifecycle.GRACEFUL_REQUEST_TIMEOUT_SEC,
    )
    server = uvicorn.Server(config)
    lifecycle.set_server(server)
    lifecycle.start_exit_watchdog(server)
    lifecycle.start_parent_watch()  # macOS / Linux: leave with the app (MAC-1)
    server.run()
    _exit_after_serve(server)


def _exit_after_serve(server) -> None:
    """Bounded wait for executor workers, flush, then hard-exit: executor
    threads are non-daemon, so the interpreter would otherwise join a still-
    busy one indefinitely at exit."""
    import logging
    from khensu_services.common import lifecycle

    lifecycle.wait_for_workers()
    logging.shutdown()
    sys.stdout.flush()
    sys.stderr.flush()
    os._exit(0 if getattr(server, "started", False) else 1)


# NOTE: warm-start is handled by the FastAPI lifespan in khensu_services.server
# (on a background thread so /health responds immediately), so this launcher no
# longer pre-warms librosa here — doing it twice doubled cold-start cost.

# The imperative startup runs only when this file is executed as a script. Under
# `import` (e.g. unit tests exercising apply_offline_env), it must NOT bootstrap
# a venv or start uvicorn.
if __name__ == "__main__":
    if "--smoke" in sys.argv:
        # Importing these is the smoke check; keeping the bindings unused is
        # intentional because a successful import is the contract here.
        import fastapi  # noqa: F401
        import librosa  # noqa: F401
        import soundfile  # noqa: F401
        import uvicorn  # noqa: F401
        from khensu_services.version import SERVICE_VERSION
        print(f"[khensu-service] runtime smoke OK (service {SERVICE_VERSION})", flush=True)
        sys.exit(0)

    # ── First-run bootstrap ───────────────────────────────────────────────────
    # Only bootstrap when neither the staged runtime nor venv is executing us.
    _running_from_runtime = RUNTIME_PYTHON.exists() and Path(sys.executable).resolve() == RUNTIME_PYTHON.resolve()
    _running_from_venv = Path(sys.executable).resolve() == VENV_PYTHON.resolve()

    if not _running_from_runtime and not _running_from_venv and RUNTIME_PYTHON.exists():
        os.execv(str(RUNTIME_PYTHON), [str(RUNTIME_PYTHON)] + sys.argv)
        sys.exit(1)

    if not _running_from_runtime and not _running_from_venv and not venv_current():
        if not bootstrap_venv():
            sys.exit(3)
        VENV_PYTHON = venv_python(VENV_DIR)

    # ── Re-exec from venv Python if needed ────────────────────────────────────
    if not _running_from_runtime and not _running_from_venv and VENV_PYTHON.exists():
        os.execv(str(VENV_PYTHON), [str(VENV_PYTHON)] + sys.argv)
        sys.exit(1)  # execv never returns on success

    # ── Normal startup ────────────────────────────────────────────────────────
    # At this point we are running from the installed runtime or dev venv.
    sys.path.insert(0, str(HERE))

    # Apply the HF/transformers offline env from settings BEFORE importing any
    # app module that pulls in huggingface libs (they read these at import time).
    # A corrupt or unreadable settings file fails closed (SVC-15), never open.
    try:
        from khensu_services.common.online import load_settings, settings_corrupt
        apply_offline_env(os.environ, load_settings(), corrupt=settings_corrupt())
    except Exception:
        _set_offline(os.environ)  # never block startup on settings, but stay offline

    # Fatal native crashes (a DSP extension segfault) leave every thread's
    # Python stack in <config dir>/crashes/, next to the desktop's minidumps.
    try:
        from khensu_services.common.crash_log import enable_faulthandler, install_crash_log
        enable_faulthandler()
        # Unhandled Python exceptions (main / worker threads, and every
        # route's 500) go to service-crash.log / service-errors.log there too.
        install_crash_log()
    except Exception:
        pass  # never block startup on crash logging

    import logging as _logging
    _logging.basicConfig(level=_logging.WARNING)

    run_service()
