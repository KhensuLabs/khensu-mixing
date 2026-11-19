"""Manifest, integrity, and availability diagnostics for optional ML models.

The UI receives stable reason codes and remediation hints only. Loader errors,
paths, credentials, and tracebacks stay out of the response by design.
"""
from __future__ import annotations

import hashlib
import json
import logging
import os
import threading
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from khensu_services.common.model_paths import models_dir as _shared_models_dir
from khensu_services.common.security_log import KIND_MODEL_UNAVAILABLE, record_event

_LOG = logging.getLogger(__name__)
_LOGGED_UNAVAILABLE: set[tuple[str, str | None]] = set()

_REASONS = {
    "disabled_by_setting": "Enable this optional capability in Settings, then restart the analysis service.",
    "missing_artifact": "Install or train the optional model, then select it in Settings if needed.",
    "missing_dependency": "Install the optional ML runtime (pip install -e '.[onnx]').",
    "artifact_digest_mismatch": "Replace the model with a verified artifact from its trusted source.",
    "incompatible_artifact": "Replace the model with a compatible exported artifact.",
}

# Every first-party, locally managed optional model appears here even before its
# artifact has been installed. That lets operators distinguish intentional
# opt-out from a missing package or rejected model instead of seeing silence.
_MODEL_SPECS: dict[str, dict[str, str | None]] = {
    "artifact_model": {"filename": "artifact_model.onnx", "enable_env": "KHENSU_ENABLE_LEARNED_ARTIFACT",
                       "path_env": "KHENSU_ARTIFACT_MODEL", "format": "onnx"},
    "ai_detector": {"filename": "ai_detector.onnx", "enable_env": "KHENSU_ENABLE_AI_DETECTOR",
                    "path_env": "KHENSU_AI_DETECTOR_MODEL", "format": "onnx"},
    "ai_attribution": {"filename": "ai_attribution.onnx", "enable_env": "KHENSU_ENABLE_AI_DETECTOR",
                       "path_env": "KHENSU_ATTRIBUTION_MODEL", "format": "onnx"},
    "mix_quality": {"filename": "mix_quality.onnx", "enable_env": "KHENSU_ENABLE_MIX_QUALITY",
                    "path_env": "KHENSU_MIX_QUALITY_MODEL", "format": "onnx"},
    "leak_gate": {"filename": "leak_gate.onnx", "enable_env": "KHENSU_ENABLE_LEAK_MODEL",
                  "path_env": "KHENSU_LEAK_MODEL", "format": "onnx"},
    "mood_tagger": {"filename": "mood_tagger.json", "enable_env": None,
                    "path_env": "KHENSU_MOOD_TAGGER_MODEL", "format": "json"},
    # NEW-9: linear heads on the CLAP embedding (learned/clap_probes.py); they
    # run only when CLAP itself can run offline.
    "clap_mood_probe": {"filename": "clap_mood_probe.json", "enable_env": None,
                        "path_env": None, "format": "json"},
    "clap_ai_probe": {"filename": "clap_ai_probe.json", "enable_env": None,
                      "path_env": None, "format": "json"},
    # Key/tempo ONNX (onnx_inference.py) - registered so it is pinned and
    # reported like every other model (AID-14). Off unless the file exists.
    "key_tempo": {"filename": "key_tempo.onnx", "enable_env": None,
                  "path_env": None, "format": "onnx"},
}


def _models_dir() -> Path:
    # The test seam (monkeypatched by the suite); the location itself comes
    # from the one shared helper, common/model_paths.models_dir().
    return _shared_models_dir()


def models_dir() -> Path:
    """The one bundled-models directory (``local-services/resources/models``).

    Every loader should resolve bundled models through this helper (AID-09:
    a hand-rolled ``parents[3]`` in one loader pointed at the repo root, so its
    model never loaded while the manifest reported it available)."""
    return _models_dir()


def bundled_model_path(filename: str, path_env: str | None = None) -> Path | None:
    """Resolve a model: the ``path_env`` override when set and present, else
    the bundled file in :func:`models_dir`, else None."""
    if path_env:
        env = os.environ.get(path_env, "").strip()
        if env and os.path.isfile(env):
            return Path(env)
    candidate = models_dir() / filename
    return candidate if candidate.is_file() else None


def _registry_path() -> Path:
    return Path(__file__).resolve().parent / "model_registry.json"


def load_registry() -> dict[str, Any]:
    """Load bundled model provenance, returning an empty registry on error."""
    try:
        data = json.loads(_registry_path().read_text(encoding="utf-8"))
        return data if isinstance(data, dict) else {}
    except (OSError, ValueError):
        return {}


_DIGEST_CACHE: dict[tuple[str, int, int], str] = {}
_DIGEST_LOCK = threading.Lock()


def _sha256(path: Path) -> str:
    """SHA-256 of `path`, memoised on (path, mtime, size): detectors verify on
    every load and the manifest on every request."""
    st = path.stat()
    key = (str(path.resolve()), st.st_mtime_ns, st.st_size)
    with _DIGEST_LOCK:
        hit = _DIGEST_CACHE.get(key)
    if hit is not None:
        return hit
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    digest = h.hexdigest()
    with _DIGEST_LOCK:
        if len(_DIGEST_CACHE) > 64:
            _DIGEST_CACHE.clear()
        _DIGEST_CACHE[key] = digest
    return digest


def _enabled(env_name: str | None) -> bool:
    return env_name is None or os.environ.get(env_name, "").strip().lower() in ("1", "true", "yes")


def _artifact_path(name: str, spec: dict[str, str | None], models_dir: Path) -> Path | None:
    configured = os.environ.get(spec.get("path_env") or "", "").strip()
    if configured:
        candidate = Path(configured)
        return candidate if candidate.is_file() else None
    candidate = models_dir / str(spec["filename"])
    return candidate if candidate.is_file() else None


def _sidecar(path: Path) -> dict[str, Any]:
    sidecar = path.with_suffix(".json")
    if not sidecar.is_file():
        return {}
    try:
        value = json.loads(sidecar.read_text(encoding="utf-8"))
        return value if isinstance(value, dict) else {}
    except (OSError, ValueError):
        return {}


def _pins_path() -> Path:
    return Path(__file__).resolve().parent / "model_pins.json"


def load_pins() -> dict[str, str]:
    """{model name: sha256} from the repo-controlled model_pins.json."""
    try:
        data = json.loads(_pins_path().read_text(encoding="utf-8"))
        pins = data.get("sha256", {}) if isinstance(data, dict) else {}
        return {str(k): str(v).lower() for k, v in pins.items() if isinstance(v, str)}
    except (OSError, ValueError, AttributeError):
        return {}


def registry_pin(name: str) -> str | None:
    """The repo-controlled SHA-256 pin for model `name`, or None."""
    pin = load_pins().get(name)
    return pin if pin and len(pin) == 64 else None


def _is_bundled(path: Path) -> bool:
    try:
        return Path(path).resolve().parent == models_dir().resolve()
    except OSError:
        return False


def digest_status(path: Path, name: str | None = None) -> str | None:
    """Why `path` must not be loaded, or None when its digest checks out.

    AID-14: the check used to trust only an ``expected_sha256`` in the sidecar
    *beside* the model (self-attested) and passed when it was missing - none of
    the installed sidecars had one. Now:

    * a bundled model (in :func:`models_dir`) must have a pin in the
      repo-controlled ``model_pins.json`` and match it - fail closed otherwise,
      including when there is no pin (SHIP-5);
    * any model whose sidecar declares ``expected_sha256`` must match it;
    * an operator-supplied path (``KHENSU_*_MODEL``) with no declared digest is
      accepted but recorded as unverified in the security log.
    """
    path = Path(path)
    name = name or path.stem
    try:
        actual = _sha256(path)
    except OSError:
        return "artifact unreadable"
    if _is_bundled(path):
        pin = registry_pin(name)
        if pin is None:
            # SHIP-5: fail closed. A file in the bundled folder is trusted
            # only once its digest is a reviewed repo diff (pin_model).
            return "bundled model has no registry SHA-256 pin"
        if pin != actual:
            return "bundled model does not match its registry SHA-256 pin"
    expected = _sidecar(path).get("expected_sha256")
    if isinstance(expected, str) and expected and expected.lower() != actual:
        return "model does not match the digest its sidecar declares"
    if not _is_bundled(path) and not (isinstance(expected, str) and expected):
        key = (name, "unverified_external")
        if key not in _LOGGED_UNAVAILABLE:
            _LOGGED_UNAVAILABLE.add(key)
            record_event(KIND_MODEL_UNAVAILABLE, f"model={name} reason=unverified_external_path")
    return None


def artifact_digest_valid(path: Path, name: str | None = None) -> bool:
    """True when `path` passes :func:`digest_status` (registry pin for
    bundled models, sidecar digest when declared)."""
    return digest_status(path, name) is None


def pin_model(name: str, path: Path) -> str:
    """Record `path`'s SHA-256 as model `name`'s pin in model_pins.json
    (trainers call this after exporting into the bundled models dir, so the
    new pin shows up as a reviewed repo diff). Returns the digest."""
    digest = _sha256(Path(path))
    try:
        data = json.loads(_pins_path().read_text(encoding="utf-8"))
        if not isinstance(data, dict):
            data = {"schema": 1}
    except (OSError, ValueError):
        data = {"schema": 1}
    data.setdefault("sha256", {})[name] = digest
    _pins_path().write_text(json.dumps(data, indent=2) + chr(10), encoding="utf-8")
    return digest


def _availability(name: str, spec: dict[str, str | None] | None, path: Path | None,
                  metadata: dict[str, Any], sha256: str | None) -> tuple[bool, str | None, str | None]:
    if spec and not _enabled(spec.get("enable_env")):
        return False, "disabled_by_setting", _REASONS["disabled_by_setting"]
    if path is None:
        return False, "missing_artifact", _REASONS["missing_artifact"]
    expected_digest = metadata.get("expected_sha256")
    if isinstance(expected_digest, str) and expected_digest and expected_digest != sha256:
        return False, "artifact_digest_mismatch", _REASONS["artifact_digest_mismatch"]
    if _is_bundled(path):
        pin = registry_pin(name)
        if pin is not None and pin != sha256:
            return False, "artifact_digest_mismatch", _REASONS["artifact_digest_mismatch"]
    try:
        if spec and spec.get("format") == "json":
            artifact = json.loads(path.read_text(encoding="utf-8"))
            if not isinstance(artifact, dict) or not {"scaler_mean", "scaler_scale", "tag_models"} <= artifact.keys():
                raise ValueError("invalid mood-tagger schema")
        else:
            get_onnx_session(path)
    except ImportError:
        return False, "missing_dependency", _REASONS["missing_dependency"]
    except Exception:
        return False, "incompatible_artifact", _REASONS["incompatible_artifact"]
    return True, None, None


_SESSIONS: dict[tuple[str, int, int], Any] = {}
_SESSION_LOCK = threading.Lock()


def get_onnx_session(path: Path):
    """Cached CPU onnxruntime session for `path` (AID-14): building a session
    per call re-parsed the model every scan. Keyed on (path, mtime, size) so a
    replaced file is reloaded; built under a lock so concurrent scans share
    one. Raises ImportError when onnxruntime is missing.

    The execution providers come from ``common.accel`` (CE-PLT-1: DirectML /
    CUDA / CoreML when the Settings preference allows and one is available,
    else CPU); they are part of the cache key so changing the preference
    builds a new session. A GPU provider that fails to initialise falls back
    to a CPU session rather than failing the scan."""
    import onnxruntime as ort
    from khensu_services.common.accel import CPU_ONNX_PROVIDER, onnx_providers
    p = Path(path)
    st = p.stat()
    providers = tuple(onnx_providers())
    key = (str(p.resolve()), st.st_mtime_ns, st.st_size, providers)
    with _SESSION_LOCK:
        sess = _SESSIONS.get(key)
        if sess is None:
            for stale in [k for k in _SESSIONS if k[0] == key[0]]:
                _SESSIONS.pop(stale, None)
            try:
                sess = ort.InferenceSession(str(p), providers=list(providers))
            except Exception:
                if providers == (CPU_ONNX_PROVIDER,):
                    raise
                sess = ort.InferenceSession(str(p), providers=[CPU_ONNX_PROVIDER])
            _SESSIONS[key] = sess
        return sess


def _registry_fields(name: str) -> dict[str, Any]:
    reg = load_registry().get("models", {}).get(name)
    if not isinstance(reg, dict):
        return {}
    return {
        "provenance": reg.get("provenance"),
        "source_url": reg.get("source_url") or None,
        "registry": {key: reg.get(key) for key in ("description", "produced_by", "license")},
    }


def _entry(name: str, path: Path | None, spec: dict[str, str | None] | None) -> dict[str, Any]:
    metadata = _sidecar(path) if path is not None and path.suffix == ".onnx" else {}
    sha256 = _sha256(path) if path is not None else None
    available, code, hint = _availability(name, spec, path, metadata, sha256)
    entry: dict[str, Any] = {
        "name": name,
        "filename": path.name if path is not None else (spec or {}).get("filename", f"{name}.onnx"),
        "installed": path is not None,
        "available": available,
        "unavailable_reason": code,
        "remediation_hint": hint,
        "intervention_validated": bool(metadata.get("intervention_validated", False)),
        "provenance": metadata.get("provenance"),
        "version": metadata.get("version"),
        "source_url": metadata.get("source_url"),
    }
    if path is not None:
        stat = path.stat()
        entry.update({
            "size_bytes": stat.st_size,
            "sha256": sha256,
            "downloaded_at": datetime.fromtimestamp(stat.st_mtime, tz=timezone.utc).isoformat(timespec="seconds"),
        })
    if metadata:
        entry["metadata"] = metadata
        for key in ("val_accuracy", "val_ece", "expected_calibration_error"):
            if key in metadata:
                entry[key] = metadata[key]
    registry = _registry_fields(name)
    for key, value in registry.items():
        if entry.get(key) in (None, ""):
            entry[key] = value
    if not available:
        # The detail is deliberately a fixed name/code pair: no paths or loader
        # exception text may enter diagnostics or UI-facing logs.
        key = (name, code)
        if key not in _LOGGED_UNAVAILABLE:
            _LOGGED_UNAVAILABLE.add(key)
            record_event(KIND_MODEL_UNAVAILABLE, f"model={name} reason={code}")
            _LOG.warning("model_unavailable model=%s reason=%s", name, code)
    return entry


def build_manifest() -> dict[str, Any]:
    """Return installed and expected optional models with redacted availability."""
    models_dir = _models_dir()
    actual: dict[str, Path] = {}
    if models_dir.is_dir():
        for path in sorted(models_dir.glob("*.onnx")):
            actual[path.stem] = path
        for name in ("mood_tagger", "clap_mood_probe", "clap_ai_probe"):
            json_model = models_dir / f"{name}.json"
            if json_model.is_file():
                actual[name] = json_model

    models: list[dict[str, Any]] = []
    for name, path in actual.items():
        models.append(_entry(name, path, _MODEL_SPECS.get(name)))
    for name, spec in _MODEL_SPECS.items():
        if name not in actual:
            models.append(_entry(name, _artifact_path(name, spec, models_dir), spec))
    return {
        "models_dir": str(models_dir),
        "count": len(actual),
        "available_count": sum(1 for model in models if model["available"]),
        "unavailable_count": sum(1 for model in models if not model["available"]),
        "any_unvalidated": any(not model["intervention_validated"] for model in models if model["installed"]),
        "models": models,
    }
