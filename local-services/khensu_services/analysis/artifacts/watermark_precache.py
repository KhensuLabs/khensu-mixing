# local-services/khensu_services/analysis/artifacts/watermark_precache.py
"""Pre-cache the learned-watermark model weights (AudioSeal + WavMark).

The AudioSeal and WavMark detectors download their weights from the HuggingFace
Hub on first load. Left on the analysis thread that download hung `full_intake`
when it stalled/rate-limited, so both tiers are gated opt-out behind
``KHENSU_ENABLE_WATERMARK_MODELS`` (see ``audioseal_tier``/``wavmark_tier``).

This module downloads those same weights *ahead of time*, off the analysis
path — from a CLI at install/first-run, or from the ``/models/watermark/*``
endpoints (which the desktop Settings can drive with progress). Because the
pre-cache calls the identical pinned + SHA-256-verified loaders the tiers use
(``safe_weights``, AUDIT-012), the weights land in the same HuggingFace cache the gated tiers read, so
once pre-cached, flipping the toggle loads from disk with no network I/O and no
hang.

Pre-caching is deliberately separate from enabling detection: it removes the
hang, but the opt-in flag stays the explicit switch (the detectors are
probabilistic and uncalibrated — 2026-07-15 safety audit).

CLI: ``python -m khensu_services.analysis.artifacts.watermark_precache``
"""
from __future__ import annotations

import contextlib
import json
import threading
import time
from pathlib import Path
from khensu_services.common.job_status import JobStatus

from khensu_services.common.online import hf_downloads_allowed  # noqa: F401  (re-exported)

_MODELS = ("audioseal", "wavmark")


def _models_root() -> Path:
    """Managed model dir, shared with the demucs ``ModelDownloader`` so a
    user-chosen models location also holds the pre-cache sentinel."""
    root: Path | None = None
    try:
        from khensu_services.copilot.downloader import ModelDownloader
        md = ModelDownloader()
        md.load_config()
        if md.models_dir:
            root = Path(md.models_dir)
    except Exception:
        root = None
    if root is None:
        from khensu_services.auth import get_token_path
        root = get_token_path().parent / "models"
    root.mkdir(parents=True, exist_ok=True)
    return root


def _sentinel_path() -> Path:
    return _models_root() / "watermark_precache.json"


def read_sentinel() -> dict:
    """Recorded pre-cache results ({model: {cached, at, error}}); {} if none."""
    p = _sentinel_path()
    if not p.exists():
        return {}
    try:
        with open(p, "r", encoding="utf-8") as f:
            data = json.load(f)
        return data if isinstance(data, dict) else {}
    except Exception:
        return {}


def _write_sentinel(results: dict) -> None:
    try:
        with open(_sentinel_path(), "w", encoding="utf-8") as f:
            json.dump(results, f, indent=2)
    except Exception:
        pass


def is_cached() -> dict:
    """Per-model cached flag from the sentinel. Sentinel-based (not an HF-cache
    probe) so it stays truthful without depending on huggingface_hub internals;
    a cleared cache would at worst trigger one re-download on enable."""
    s = read_sentinel()
    return {m: bool(s.get(m, {}).get("cached")) for m in _MODELS}


def gate_open(model_key: str) -> bool:
    """Whether the tier for ``model_key`` ("audioseal"/"wavmark") may load now.

    Opens when either the explicit opt-in flag is set, OR the weights are already
    pre-cached — in the cached case the load is fast and offline, so there's no
    HF-download hang to guard against and the user needn't set the env var by
    hand. Sentinel-based (see ``is_cached``); a cleared cache would at worst
    cause one failed load that the tier records and stops retrying.
    """
    import os
    if os.environ.get("KHENSU_ENABLE_WATERMARK_MODELS", "").strip().lower() in ("1", "true", "yes"):
        return True
    return bool(is_cached().get(model_key))


def _hf_constants():
    """``huggingface_hub.constants`` if the package is installed, else None."""
    try:
        import huggingface_hub.constants as hf_constants
        return hf_constants
    except Exception:
        return None


@contextlib.contextmanager
def hf_offline_if_downloads_disabled():
    """Force huggingface_hub into offline mode for the duration of a model
    load when model downloads are disabled in Settings (audit SEC-02, parity
    with the demucs cached-only semantics).

    With downloads off, a cache *miss* must fail closed rather than silently
    fetch weights from the network — so offline mode is forced here and the
    loader either loads from the existing cache or raises. No-op when downloads
    are allowed (the normal / explicit-opt-in path, where a first-run download
    is intended).

    huggingface_hub snapshots ``HF_HUB_OFFLINE`` from the environment into
    ``huggingface_hub.constants.HF_HUB_OFFLINE`` at *import* time, so setting
    the env var alone is ineffective once the package is loaded. Both the env
    vars (for anything imported later / subprocesses) and the live module
    constant are set, and all are restored afterwards.
    """
    if hf_downloads_allowed():
        yield
        return
    # AID-18: two loads swapping the process-wide env/constant concurrently
    # could restore each other's "prior" value and leave HF_HUB_OFFLINE stuck
    # on. The whole swap-load-restore is serialized (re-entrant for nesting).
    with _HF_ENV_LOCK:
        with _hf_offline_swap():
            yield


_HF_ENV_LOCK = threading.RLock()


@contextlib.contextmanager
def _hf_offline_swap():
    import os
    keys = ("HF_HUB_OFFLINE", "TRANSFORMERS_OFFLINE")
    prior = {k: os.environ.get(k) for k in keys}
    for k in keys:
        os.environ[k] = "1"
    hf_constants = _hf_constants()
    _missing = object()
    prior_const = getattr(hf_constants, "HF_HUB_OFFLINE", _missing) if hf_constants else _missing
    if hf_constants is not None:
        hf_constants.HF_HUB_OFFLINE = True
    try:
        yield
    finally:
        if hf_constants is not None:
            if prior_const is _missing:
                try:
                    delattr(hf_constants, "HF_HUB_OFFLINE")
                except AttributeError:
                    pass
            else:
                hf_constants.HF_HUB_OFFLINE = prior_const
        for k, v in prior.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v


def load_watermark_model(model_key: str, loader):
    """Runs a watermark model ``loader`` under the cached-only guard above
    (audit SEC-02). Returns the loaded model, or ``None`` if it couldn't load
    offline while downloads are disabled — recording a single
    ``KIND_ONLINE_GATE_BLOCKED`` event in that case (the load was refused a
    network fetch), matching the demucs separator's behavior. Re-raises for any
    other (non-offline) failure so the caller's existing error handling runs.
    """
    downloads_allowed = hf_downloads_allowed()
    try:
        with hf_offline_if_downloads_disabled():
            return loader()
    except Exception as exc:
        if not downloads_allowed:
            from khensu_services.common.security_log import KIND_ONLINE_GATE_BLOCKED, record_event
            record_event(KIND_ONLINE_GATE_BLOCKED,
                         f"watermark model '{model_key}' not cached and downloads disabled")
            return None
        raise exc


def packages_available() -> dict:
    """Whether each detector's Python package is importable at all."""
    import importlib.util
    return {
        "audioseal": importlib.util.find_spec("audioseal") is not None,
        "wavmark": importlib.util.find_spec("wavmark") is not None,
    }


def precache_audioseal() -> tuple[bool, str]:
    """Download (pinned revision) + verify + safely load the AudioSeal
    detector (AUDIT-012). Never raises."""
    try:
        from khensu_services.analysis.artifacts.safe_weights import build_audioseal_detector
        build_audioseal_detector(allow_download=True)
        return True, ""
    except Exception as exc:  # missing package, offline, HF error, hash mismatch, ...
        return False, str(exc)


def precache_wavmark() -> tuple[bool, str]:
    """Download (pinned revision) + verify + safely load WavMark. Never raises."""
    try:
        from khensu_services.analysis.artifacts.safe_weights import build_wavmark_model
        build_wavmark_model(allow_download=True)
        return True, ""
    except Exception as exc:
        return False, str(exc)


_PRECACHERS = {"audioseal": precache_audioseal, "wavmark": precache_wavmark}


def precache_all(progress_cb=None) -> dict:
    """Pre-cache every watermark model, merging into (not clobbering) the
    sentinel so a prior success survives a later partial run. Returns the
    per-model result dict. Synchronous — call off the request/analysis thread.

    progress_cb, if given, receives (model_name, done_index, total).
    """
    results = read_sentinel()
    total = len(_PRECACHERS)
    for i, (name, fn) in enumerate(_PRECACHERS.items()):
        ok, err = fn()
        results[name] = {"cached": ok, "at": time.time() if ok else results.get(name, {}).get("at"),
                         "error": "" if ok else err}
        if progress_cb is not None:
            try:
                progress_cb(name, i + 1, total)
            except Exception:
                pass
    _write_sentinel(results)
    return results


class WatermarkPrecacher:
    """Singleton background runner so the HTTP layer can start a pre-cache and
    poll its status, mirroring the demucs ``ModelDownloader`` pattern."""

    _instance = None
    _lock = threading.Lock()

    def __new__(cls):
        with cls._lock:
            if cls._instance is None:
                cls._instance = super().__new__(cls)
                cls._instance._init()
            return cls._instance

    def _init(self) -> None:
        self.status = "idle"          # idle | downloading | completed | failed
        self.current_model = ""
        self.done = 0
        self.total = len(_MODELS)
        self.error = ""
        self.thread: threading.Thread | None = None

    def start(self) -> None:
        with self._lock:
            if self.status == "downloading":
                return
            self.status = "downloading"
            self.current_model = ""
            self.done = 0
            self.error = ""
            self.thread = threading.Thread(target=self._run, daemon=True)
            self.thread.start()

    def _run(self) -> None:
        def _cb(name: str, done: int, total: int) -> None:
            self.current_model = name
            self.done = done
            self.total = total
        try:
            results = precache_all(progress_cb=_cb)
            failed = {m: r.get("error", "") for m, r in results.items()
                      if m in _MODELS and not r.get("cached")}
            if failed:
                self.status = JobStatus.FAILED.value
                self.error = "; ".join(f"{m}: {e}" for m, e in failed.items())
            else:
                self.status = JobStatus.COMPLETED.value
        except Exception as exc:  # defensive — precache_all already guards
            self.status = JobStatus.FAILED.value
            self.error = str(exc)

    def status_dict(self) -> dict:
        return {
            "status": self.status,
            "current_model": self.current_model,
            "done": self.done,
            "total": self.total,
            "error": self.error,
            "packages_available": packages_available(),
            "cached": is_cached(),
            "sentinel": read_sentinel(),
        }


def status() -> dict:
    """Full status without necessarily having started a job."""
    return WatermarkPrecacher().status_dict()


def _main() -> int:
    avail = packages_available()
    missing = [m for m, ok in avail.items() if not ok]
    if missing:
        print(f"watermark packages not installed: {', '.join(missing)} "
              f"(install with: pip install -e \".[ai_detect]\")")
    print("Pre-caching watermark model weights (AudioSeal + WavMark)...")
    results = precache_all(progress_cb=lambda n, d, t: print(f"  [{d}/{t}] {n}"))
    print(json.dumps(results, indent=2))
    all_ok = all(results.get(m, {}).get("cached") for m in _MODELS)
    print("OK — weights cached; enable with KHENSU_ENABLE_WATERMARK_MODELS=1"
          if all_ok else "Some models failed to cache (see errors above).")
    return 0 if all_ok else 1


if __name__ == "__main__":
    import sys
    sys.exit(_main())
