"""Learned beat and downbeat tracking (NEW-6).

The librosa tracker finds a beat grid from onset strength and a tempo
estimate; the meter (``estimate_meter``) and octave folding
(``_shared_analysis``) are heuristics on top. Beat This! (Foscarin, Schlüter
& Widmer, ISMIR 2024; MIT) predicts beats and downbeats directly from a
log-mel spectrogram, with no tempo prior, so half/double-tempo locks and
pickup bars are what it was trained to get right. The ``small0`` model runs
as ONNX on the main environment's onnxruntime
(``resources/models/beat_tracker.onnx``, exported and pinned by
``scripts/export_beat_this_onnx.py``); without it, callers keep the librosa
tracker.

The front end matches the package's torchaudio one (checked at export): 22.05
kHz mono, 1024-point STFT, hop 441 (50 frames/s), magnitude / sqrt(n_fft),
128 Slaney mel bands 30 Hz-11 kHz without area normalisation,
log1p(1000 * mel). Long tracks are cut into 1500-frame chunks overlapping by
6 frames each side, as in ``split_predict_aggregate``; the peaks are the
"minimal" post-processing (logit maxima within +-70 ms above 0, adjacent
peaks merged, each downbeat moved to its nearest beat).
"""
from __future__ import annotations

import threading
from typing import Any

import numpy as np
import numpy.typing as npt

MODEL_FILE = "beat_tracker.onnx"
MODEL_ID = "beat_tracker"
SR = 22050
N_FFT = 1024
HOP = 441
FPS = SR / HOP  # 50
N_MELS = 128
_CHUNK = 1500
_BORDER = 6
_lock = threading.Lock()
_mel_fb: npt.NDArray | None = None


def model_available() -> bool:
    try:
        import onnxruntime  # noqa: F401
    except ImportError:
        return False
    from khensu_services.analysis.model_manifest import bundled_model_path, digest_status
    path = bundled_model_path(MODEL_FILE, "KHENSU_BEAT_MODEL")
    return path is not None and not digest_status(path, MODEL_ID)


def _filterbank() -> npt.NDArray:
    global _mel_fb
    if _mel_fb is None:
        import librosa
        _mel_fb = librosa.filters.mel(sr=SR, n_fft=N_FFT, n_mels=N_MELS, fmin=30.0, fmax=11000.0,
                                      htk=False, norm=None).astype(np.float32)
    return _mel_fb


def log_mel(y22: npt.NDArray) -> npt.NDArray:
    """(frames, 128) log-mel of mono 22.05 kHz audio, as Beat This! expects."""
    y = np.asarray(y22, dtype=np.float32)
    pad = N_FFT // 2
    yp = np.pad(y, (pad, pad), mode="reflect") if len(y) > pad else np.pad(y, (pad, pad))
    n = 1 + (len(yp) - N_FFT) // HOP
    win = (0.5 - 0.5 * np.cos(2 * np.pi * np.arange(N_FFT) / N_FFT)).astype(np.float32)  # periodic Hann
    out = np.empty((n, N_MELS), dtype=np.float32)
    fb = _filterbank()
    for s in range(0, n, 2048):
        idx = np.arange(N_FFT)[None, :] + HOP * np.arange(s, min(n, s + 2048))[:, None]
        mag = np.abs(np.fft.rfft(yp[idx] * win, axis=1)) / np.sqrt(N_FFT)
        out[s:s + len(idx)] = np.log1p(1000.0 * (mag @ fb.T))
    return out


def _session():
    from khensu_services.analysis.model_manifest import bundled_model_path, get_onnx_session
    return get_onnx_session(bundled_model_path(MODEL_FILE, "KHENSU_BEAT_MODEL"))


def activations(spect: npt.NDArray) -> tuple[npt.NDArray, npt.NDArray]:
    """Beat and downbeat logits per frame for the whole piece."""
    n = len(spect)
    sess = _session()
    beat = np.full(n, -1000.0, dtype=np.float32)
    down = np.full(n, -1000.0, dtype=np.float32)
    starts = list(range(-_BORDER, n - _BORDER, _CHUNK - 2 * _BORDER))
    if n > _CHUNK - 2 * _BORDER:
        starts[-1] = n - (_CHUNK - _BORDER)
    with _lock:
        # keep_first: later chunks first, so earlier ones overwrite overlaps
        for start in reversed(starts):
            lo, hi = max(start, 0), min(start + _CHUNK, n)
            chunk = spect[lo:hi]
            left = max(0, -start)
            right = max(0, min(_BORDER, start + _CHUNK - n))
            chunk = np.pad(chunk, ((left, right), (0, 0)))
            b, d = sess.run(None, {"spect": chunk[None].astype(np.float32)})
            b, d = b[0][_BORDER:-_BORDER], d[0][_BORDER:-_BORDER]
            a = start + _BORDER
            k = min(len(b), n - a)
            if k > 0:
                beat[a:a + k] = b[:k]
                down[a:a + k] = d[:k]
    return beat, down


def _dedupe(frames: npt.NDArray, width: int = 1) -> npt.NDArray:
    out: list[float] = []
    it = iter(int(f) for f in frames)
    try:
        p: float = next(it)
    except StopIteration:
        return np.array([])
    c = 1
    for p2 in it:
        if p2 - p <= width:
            c += 1
            p += (p2 - p) / c
        else:
            out.append(p)
            p, c = p2, 1
    out.append(p)
    return np.array(out)


def peaks(logits: npt.NDArray) -> npt.NDArray:
    """Frames that are the maximum within +-3 frames (70 ms) and above 0."""
    padded = np.pad(logits, 3, constant_values=-np.inf)
    win = np.lib.stride_tricks.sliding_window_view(padded, 7).max(axis=1)
    return _dedupe(np.nonzero((logits == win) & (logits > 0))[0])


def track(y: npt.NDArray, sr: int) -> dict[str, Any]:
    """Beats, downbeats (s), tempo and beats per bar of ``y`` (mono or
    (channels, samples)). Raises when the model is unavailable."""
    import librosa

    mono = np.asarray(y.mean(axis=0) if y.ndim == 2 else y, dtype=np.float32)
    y22 = librosa.resample(mono, orig_sr=sr, target_sr=SR) if sr != SR else mono
    b, d = activations(log_mel(y22))
    beat_t = peaks(b) / FPS
    down_t = peaks(d) / FPS
    if len(beat_t):
        down_t = np.unique([beat_t[int(np.argmin(np.abs(beat_t - x)))] for x in down_t])
    bpm = float(60.0 / np.median(np.diff(beat_t))) if len(beat_t) > 2 else 0.0
    per_bar = 4
    if len(down_t) > 2 and len(beat_t) > 4:
        counts = [int(np.sum((beat_t >= a) & (beat_t < b2))) for a, b2 in zip(down_t[:-1], down_t[1:])]
        if counts:
            per_bar = int(np.bincount(counts).argmax()) or 4
    return {"beats": beat_t.tolist(), "downbeats": [float(x) for x in down_t], "bpm": bpm,
            "beats_per_bar": per_bar, "backend": "beat_this_small_onnx"}
