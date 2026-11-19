"""ONNX inference for the learned AI-artifact tier.

Production inference path: forensic features (numpy) → onnxruntime session →
per-class logits → temperature-calibrated probabilities. Never imports torch.

If no exported model is present (the default until a corpus is trained), every
entry point degrades to "unavailable" and the detector no-ops — so shipping
without weights changes nothing at runtime.

Model discovery order:
  1. ``KHENSU_ARTIFACT_MODEL`` env var (an .onnx path),
  2. ``resources/models/artifact_model.onnx`` bundled with the service.

A sidecar ``<model>.json`` may carry calibration metadata:
  {"temperature": float, "thresholds": {class: float}, "intervention_validated": bool,
   "val_ece": float}
Confidence stays UNEXPOSED until ``intervention_validated`` is true (safety
audit gate) — until then the detector may surface *evidence* but not a numeric
confidence.

Audit 2026-11-06:
  * AID-12 - :class:`OnnxModelHandle` loads under a lock and only marks itself
    loaded once the session exists (a concurrent scan used to see ``_loaded``
    before the session and silently skip the model); ``unavailable_reason()``
    tells the engine *why* a model did not run, and inference errors propagate
    instead of reading as "checked, clean".
  * AID-13 - training featurizes 4 s clips (``train_sonics._featurize``), so
    inference now does the same: the track is cut into 4 s windows, each is
    featurized and scored on its own, and the per-window probabilities are
    aggregated. (It used to featurize the whole track - whole-track HPSS and
    whole-track normalisation - and then judge only the centre ~6 s.)
  * AID-14 - sessions are cached and models are digest-checked against the
    repo-controlled registry pins (``model_manifest.digest_status``).
"""
from __future__ import annotations

import json
import threading
from pathlib import Path
from typing import Any, Callable

import numpy as np

from khensu_services.analysis.learned.features import _SR, extract_forensic_features
from khensu_services.analysis.learned.model import ARTIFACT_CLASSES

# A fixed time-window the model consumes (frames); features are cropped/tiled.
_WINDOW_FRAMES = 256
# Training clip length (train_sonics._featurize: 4 s at 22.05 kHz).
WINDOW_SEC = 4.0
# Upper bound on windows scored per track (evenly spread); bounds latency.
MAX_WINDOWS = 12


def model_path() -> Path | None:
    from khensu_services.analysis.model_manifest import bundled_model_path
    return bundled_model_path("artifact_model.onnx", "KHENSU_ARTIFACT_MODEL")


def model_available() -> bool:
    mp = model_path()
    if mp is None:
        return False
    from khensu_services.analysis.model_manifest import artifact_digest_valid
    if not artifact_digest_valid(mp):
        return False
    try:
        import onnxruntime  # noqa: F401
    except Exception:
        return False
    return True


def _load_calibration(mp: Path) -> dict[str, Any]:
    sidecar = mp.with_suffix(".json")
    if sidecar.is_file():
        try:
            return json.loads(sidecar.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            pass
    return {"temperature": 1.0, "thresholds": {}, "intervention_validated": False}


def calibration_ece(calib: dict[str, Any]) -> float:
    """The held-out ECE from a sidecar. Trainers write ``val_ece``; older
    sidecars used ``ece`` (AID-13: reading only ``ece`` showed 1.000 for a
    model whose real ECE is 0.018). Missing -> 1.0 (untrusted)."""
    for key in ("val_ece", "ece", "expected_calibration_error"):
        v = calib.get(key)
        try:
            if v is not None and np.isfinite(float(v)):
                return float(v)
        except (TypeError, ValueError):
            continue
    return 1.0


def _window(feats: np.ndarray) -> np.ndarray:
    """Crop or tile the time axis to exactly _WINDOW_FRAMES."""
    t = feats.shape[2]
    if t == _WINDOW_FRAMES:
        return feats
    if t > _WINDOW_FRAMES:
        start = (t - _WINDOW_FRAMES) // 2
        return feats[:, :, start:start + _WINDOW_FRAMES]
    reps = int(np.ceil(_WINDOW_FRAMES / max(t, 1)))
    return np.tile(feats, (1, 1, reps))[:, :, :_WINDOW_FRAMES]


def window_starts(n_samples: int, sr: int = _SR, win_sec: float = WINDOW_SEC,
                  max_windows: int = MAX_WINDOWS) -> list[int]:
    """Start samples of the scored windows: one window for a short signal,
    else up to `max_windows` evenly spread non-overlapping-or-less windows."""
    length = int(win_sec * sr)
    if n_samples <= length:
        return [0]
    n = min(max_windows, int(np.ceil(n_samples / length)))
    return [int(s) for s in np.linspace(0, n_samples - length, n).round()]


def windowed_features(y: np.ndarray, sr: int, win_sec: float = WINDOW_SEC,
                      max_windows: int = MAX_WINDOWS) -> list[np.ndarray]:
    """Per-window feature tensors, each (1, 7, N_MELS, _WINDOW_FRAMES), built
    exactly like training: resample to 22.05 kHz, cut a 4 s clip, featurize
    the clip on its own (its own HPSS + normalisation), crop/tile."""
    y = np.asarray(y, dtype=np.float32)
    if sr != _SR and y.size:
        import librosa
        y = librosa.resample(y, orig_sr=sr, target_sr=_SR).astype(np.float32)
    length = int(win_sec * _SR)
    out = []
    for s in window_starts(y.size, _SR, win_sec, max_windows):
        seg = y[s:s + length]
        out.append(_window(extract_forensic_features(seg, _SR)).astype(np.float32)[None, ...])
    return out


class OnnxModelHandle:
    """Thread-safe, lazily-loaded ONNX model + sidecar.

    ``ensure()`` loads at most once per process (until the file changes), under
    a lock, and marks itself loaded only after the attempt finished - so a
    concurrent caller either waits for the session or sees the recorded error.
    ``unavailable_reason()`` is cheap and never loads."""

    def __init__(self, name: str, resolver: Callable[[], Path | None]) -> None:
        self.name = name
        self._resolver = resolver
        self._lock = threading.Lock()
        self._session = None
        self._calib: dict[str, Any] = {}
        self._error: str | None = None
        self._loaded_key: tuple | None = None
        self._installed = False

    def install(self, session: Any, calib: dict[str, Any] | None = None) -> None:
        """Inject a ready session + sidecar (tests, or a caller that built the
        session itself). ``ensure()`` then reports it loaded."""
        with self._lock:
            self._session, self._calib = session, dict(calib or {})
            self._error, self._installed = None, True

    @property
    def calib(self) -> dict[str, Any]:
        return self._calib

    def _key(self, mp: Path) -> tuple:
        st = mp.stat()
        return (str(mp), st.st_mtime_ns, st.st_size)

    def unavailable_reason(self) -> str | None:
        if self._installed:
            return None if self._session is not None else "model not loaded"
        mp = self._resolver()
        if mp is None:
            return "model not installed"
        from khensu_services.analysis.model_manifest import digest_status
        bad = digest_status(mp, self.name)
        if bad:
            return bad
        try:
            import onnxruntime  # noqa: F401
        except Exception:
            return "onnxruntime not installed"
        try:
            if self._loaded_key == self._key(mp) and self._error:
                return f"model failed to load: {self._error}"
        except OSError:
            return "model not readable"
        return None

    def ensure(self) -> bool:
        if self._installed:
            return self._session is not None
        mp = self._resolver()
        if mp is None:
            return False
        try:
            key = self._key(mp)
        except OSError:
            return False
        if self._loaded_key == key:
            return self._session is not None
        with self._lock:
            if self._loaded_key == key:
                return self._session is not None
            session, calib, error = None, {}, None
            from khensu_services.analysis.model_manifest import digest_status, get_onnx_session
            bad = digest_status(mp, self.name)
            if bad:
                error = bad
            else:
                try:
                    session = get_onnx_session(mp)
                    calib = _load_calibration(mp)
                except Exception as exc:          # recorded, surfaced as not-run
                    session, error = None, f"{type(exc).__name__}"
            self._session, self._calib, self._error = session, calib, error
            self._loaded_key = key            # set only after the attempt finished
            return session is not None

    def run(self, feats: np.ndarray) -> np.ndarray:
        """Raw first output for one (1, ...) feature tensor. Raises on error."""
        if self._session is None:
            raise RuntimeError(f"{self.name} model is not loaded")
        name = self._session.get_inputs()[0].name
        return np.asarray(self._session.run(None, {name: feats})[0]).reshape(-1)

    def run_windows(self, y: np.ndarray, sr: int) -> list[np.ndarray]:
        """Raw outputs, one per 4 s window (AID-13). Raises on error."""
        return [self.run(f) for f in windowed_features(y, sr)]


class LearnedArtifactInference:
    """Lazily-loaded ONNX session; safe to construct even with no model."""

    def __init__(self) -> None:
        self._handle = OnnxModelHandle("artifact_model", model_path)

    @property
    def _calib(self) -> dict[str, Any]:
        return self._handle.calib

    def unavailable_reason(self) -> str | None:
        return self._handle.unavailable_reason()

    def _ensure(self) -> bool:
        return self._handle.ensure()

    @property
    def intervention_validated(self) -> bool:
        return bool(self._calib.get("intervention_validated", False))

    def predict(self, y: np.ndarray, sr: int) -> dict[str, float] | None:
        """Per-class calibrated probabilities (mean over 4 s windows), or None
        when the model is unavailable. Inference errors raise (AID-12)."""
        if not self._ensure():
            return None
        temp = float(self._calib.get("temperature", 1.0)) or 1.0
        probs = [1.0 / (1.0 + np.exp(-logits / temp)) for logits in self._handle.run_windows(y, sr)]
        mean = np.mean(np.stack(probs), axis=0)
        return {cls: float(mean[i]) for i, cls in enumerate(ARTIFACT_CLASSES) if i < mean.size}
