"""Learned per-frame inter-stem bleed classifier for the LeakGate effect.

Replaces the fixed envelope-threshold expander in ``_apply_leak_gate`` with a
learned per-band, per-frame bleed *probability* that drives the gain reduction —
so the gate follows what actually reads as bleed rather than a static level gate.

Same shape as the learned artifact tier: torch for training/export, ONNX for
inference (no torch at request time), and it ships **inert** — with no model or
without the ``KHENSU_ENABLE_LEAK_MODEL`` opt-in, ``predict`` returns None and
``_apply_leak_gate`` keeps its deterministic envelope behavior. A synthetic
bootstrap model (see ``leak_gate_bootstrap.py``) is functional but not validated
on real MUSDB-style leak data, so it stays opt-in (2026-07-15 safety audit).

Bands match ``_apply_leak_gate``: sub / low / mid / high / air.
"""
from __future__ import annotations

import os
from pathlib import Path

import numpy as np

BAND_EDGES = [0.0, 120.0, 300.0, 3000.0, 8000.0]   # last edge = sr at runtime
N_BANDS = 5
N_FEAT = 3                                          # per band, per frame
_HOP = 512
_N_FFT = 2048


def _band_indices(freqs: np.ndarray, sr: int) -> list[np.ndarray]:
    edges = BAND_EDGES + [float(sr) / 2.0]
    return [(freqs >= edges[i]) & (freqs < edges[i + 1]) for i in range(N_BANDS)]


def extract_features(audio: np.ndarray, sr: int) -> np.ndarray:
    """Return a (n_frames, N_BANDS*N_FEAT) per-frame feature matrix.

    Per band, per frame: log band energy normalised to the frame's total,
    the band energy relative to that band's own max over time, and spectral
    flux (temporal change). These are the cues that separate bleed (low,
    diffuse, often in bands away from the wanted signal) from wanted content.
    """
    import librosa

    y = np.asarray(audio, dtype=np.float32)
    if y.ndim > 1:
        y = y.mean(axis=0)
    if y.size < _N_FFT:
        y = np.pad(y, (0, _N_FFT - y.size))
    S = np.abs(librosa.stft(y, n_fft=_N_FFT, hop_length=_HOP)) ** 2 + 1e-9
    freqs = librosa.fft_frequencies(sr=sr, n_fft=_N_FFT)
    idx = _band_indices(freqs, sr)
    n_frames = S.shape[1]

    band_e = np.stack([S[m].sum(axis=0) for m in idx], axis=0)          # (bands, frames)
    total = band_e.sum(axis=0, keepdims=True) + 1e-9
    rel_frame = band_e / total                                          # share of frame
    rel_time = band_e / (band_e.max(axis=1, keepdims=True) + 1e-9)      # vs band max
    flux = np.zeros_like(band_e)
    if n_frames > 1:
        flux[:, 1:] = np.abs(np.diff(np.log(band_e), axis=1))
    flux = flux / (flux.max() + 1e-9)

    feats = np.concatenate([rel_frame, rel_time, flux], axis=0)         # (bands*3, frames)
    return np.nan_to_num(feats.T.astype(np.float32), nan=0.0, posinf=0.0, neginf=0.0)


def build_model():
    """A compact per-frame MLP: (N_BANDS*N_FEAT) -> per-band bleed logits."""
    import torch.nn as nn

    return nn.Sequential(
        nn.Linear(N_BANDS * N_FEAT, 64), nn.ReLU(inplace=True),
        nn.Linear(64, 64), nn.ReLU(inplace=True),
        nn.Linear(64, N_BANDS),
    )


def model_path() -> Path | None:
    """``KHENSU_LEAK_MODEL`` when set and present, else the bundled
    ``leak_gate.onnx`` (resolved through the shared models-dir helper)."""
    from khensu_services.analysis.model_manifest import bundled_model_path
    return bundled_model_path("leak_gate.onnx", "KHENSU_LEAK_MODEL")


def model_available() -> bool:
    if os.environ.get("KHENSU_ENABLE_LEAK_MODEL", "").strip().lower() not in ("1", "true", "yes"):
        return False
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


class LeakGateInference:
    def __init__(self) -> None:
        self._session = None
        self._loaded = False

    def _ensure(self) -> bool:
        if self._loaded:
            return self._session is not None
        self._loaded = True
        mp = model_path()
        if mp is None:
            return False
        from khensu_services.analysis.model_manifest import artifact_digest_valid
        if not artifact_digest_valid(mp):
            return False
        try:
            # Cached per (path, mtime, size) and shared across instances.
            from khensu_services.analysis.model_manifest import get_onnx_session
            self._session = get_onnx_session(mp)
        except Exception:
            self._session = None
        return self._session is not None

    def predict(self, audio: np.ndarray, sr: int) -> np.ndarray | None:
        """Return a (N_BANDS, n_frames) bleed-probability grid, or None."""
        if not model_available() or not self._ensure():
            return None
        feats = extract_features(audio, sr)          # (frames, N_BANDS*N_FEAT)
        try:
            name = self._session.get_inputs()[0].name
            logits = np.asarray(self._session.run(None, {name: feats})[0])  # (frames, N_BANDS)
        except Exception:
            return None
        probs = 1.0 / (1.0 + np.exp(-logits))
        return probs.T.astype(np.float32)            # (N_BANDS, frames)
