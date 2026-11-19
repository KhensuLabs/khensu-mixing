"""Monophonic vocal pitch in the main environment (NEW-7).

The vocal tools (pitch accuracy, the pitch editor, monophonic MIDI) tracked
F0 with librosa.pyin, and the better pitched-MIDI path needs basic-pitch in a
second 1.7 GB interpreter (``.venv311``). This runs CREPE-tiny - a learned
monophonic pitch model (Kim et al. 2018, MIT-licensed weights from
``torchcrepe``) exported to ONNX by ``scripts/export_crepe_onnx.py`` - on the
main environment's onnxruntime. Without the model (or onnxruntime) it falls
back to pyin, and says which ran.

``track(y, sr, fmin, fmax, hop_length)`` returns pyin's shape - (f0 Hz with
NaN where unvoiced, voiced flags, voiced probability) on the caller's hop
grid - so callers swap one call.
"""
from __future__ import annotations

import logging
import threading
from typing import Any

import numpy as np
import numpy.typing as npt

_log = logging.getLogger(__name__)

MODEL_FILE = "vocal_pitch.onnx"
MODEL_ID = "vocal_pitch"
_SR = 16000
_WINDOW = 1024
_CREPE_HOP = 160            # 10 ms frames
_VOICED_THRESHOLD = 0.5     # CREPE periodicity (peak activation) for "voiced"
_BIN_CENTS = 20.0 * np.arange(360) + 1997.3794084376191

_lock = threading.Lock()
last_backend = "none"


def model_available() -> bool:
    try:
        import onnxruntime  # noqa: F401
    except ImportError:
        return False
    from khensu_services.analysis.model_manifest import bundled_model_path, digest_status
    path = bundled_model_path(MODEL_FILE, "KHENSU_VOCAL_PITCH_MODEL")
    return path is not None and not digest_status(path, MODEL_ID)


def _session():
    from khensu_services.analysis.model_manifest import bundled_model_path, get_onnx_session
    return get_onnx_session(bundled_model_path(MODEL_FILE, "KHENSU_VOCAL_PITCH_MODEL"))


def _frames(y16: npt.NDArray) -> npt.NDArray:
    pad = np.pad(y16.astype(np.float32), (_WINDOW // 2, _WINDOW // 2))
    n = 1 + (len(pad) - _WINDOW) // _CREPE_HOP
    idx = np.arange(_WINDOW)[None, :] + _CREPE_HOP * np.arange(n)[:, None]
    fr = pad[idx]
    fr = fr - fr.mean(axis=1, keepdims=True)
    fr = fr / np.maximum(fr.std(axis=1, keepdims=True), 1e-10)
    return fr.astype(np.float32)


def _decode(act: npt.NDArray) -> tuple[npt.NDArray, npt.NDArray]:
    """Weighted average of the activation within +/-4 bins of the peak (the
    original CREPE decoding), in Hz, and the peak activation (periodicity)."""
    peak = act.argmax(axis=1)
    lo = np.clip(peak - 4, 0, 359)
    hi = np.clip(peak + 5, 1, 360)
    cents = np.empty(len(act))
    for i in range(len(act)):
        w = act[i, lo[i]:hi[i]]
        cents[i] = float((w * _BIN_CENTS[lo[i]:hi[i]]).sum() / max(w.sum(), 1e-9))
    return 10.0 * 2.0 ** (cents / 1200.0), act.max(axis=1)


def crepe_f0(y: npt.NDArray, sr: int) -> tuple[npt.NDArray, npt.NDArray, npt.NDArray]:
    """(times s, f0 Hz, periodicity) on CREPE's 10 ms grid."""
    import librosa

    mono = y.mean(axis=0) if y.ndim == 2 else y
    y16 = librosa.resample(np.asarray(mono, dtype=np.float32), orig_sr=sr, target_sr=_SR) if sr != _SR else mono
    fr = _frames(np.asarray(y16))
    sess = _session()
    name = sess.get_inputs()[0].name
    acts = []
    with _lock:  # one ONNX run at a time keeps the DSP pool's memory flat
        for start in range(0, len(fr), 2048):
            acts.append(sess.run(None, {name: fr[start:start + 2048]})[0])
    act = np.concatenate(acts) if acts else np.zeros((0, 360), np.float32)
    f0, periodicity = _decode(act)
    times = np.arange(len(f0)) * _CREPE_HOP / _SR
    return times, f0, periodicity


def track(y: npt.NDArray, sr: int, fmin: float, fmax: float, hop_length: int,
          frame_length: int = 2048) -> tuple[npt.NDArray, npt.NDArray, npt.NDArray]:
    """pyin-shaped F0 on the caller's hop grid: (f0 with NaN unvoiced,
    voiced flags, voiced probability). CREPE when its model is present, else
    pyin; ``last_backend`` says which ran."""
    global last_backend
    import librosa

    mono = np.asarray(y.mean(axis=0) if y.ndim == 2 else y, dtype=np.float32)
    n_out = 1 + len(mono) // hop_length
    if model_available():
        try:
            times, f0, per = crepe_f0(mono, sr)
            grid = np.arange(n_out) * hop_length / sr
            f0_g = np.interp(grid, times, f0) if len(times) else np.full(n_out, np.nan)
            per_g = np.interp(grid, times, per) if len(times) else np.zeros(n_out)
            # CREPE-tiny decides voicing and the octave; its 20-cent bins
            # leave up to ~8 cents of bias on a steady note, which an
            # intonation reading cannot carry. YIN on the same grid gives the
            # fine pitch wherever it agrees within 50 cents (no octave jump).
            yin = librosa.yin(mono, fmin=fmin, fmax=fmax, sr=sr, frame_length=frame_length,
                              hop_length=hop_length)[:n_out]
            if len(yin) < n_out:
                yin = np.pad(yin, (0, n_out - len(yin)), constant_values=np.nan)
            with np.errstate(divide="ignore", invalid="ignore"):
                agree = np.abs(1200.0 * np.log2(yin / f0_g)) < 50.0
            f0_g = np.where(agree, yin, f0_g)
            voiced = (per_g >= _VOICED_THRESHOLD) & (f0_g >= fmin) & (f0_g <= fmax)
            f0_g = np.where(voiced, f0_g, np.nan)
            last_backend = "crepe_tiny_onnx+yin"
            return f0_g, voiced, per_g
        except Exception:
            _log.exception("CREPE pitch failed; falling back to pyin")
    last_backend = "pyin"
    return librosa.pyin(mono, fmin=fmin, fmax=fmax, sr=sr, frame_length=frame_length,
                        hop_length=hop_length, fill_na=np.nan)


def backend_info() -> dict[str, Any]:
    return {"available": model_available(), "model": MODEL_ID, "last_backend": last_backend}
