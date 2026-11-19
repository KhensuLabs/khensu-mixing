"""7-channel forensic feature stack for the learned AI-artifact tier.

This is the deterministic feature side of the learned pipeline described in the
spec: STFT magnitude → (bounded-mask residual) → median-filter HPSS → a
7-channel forensic representation that the ArtifactUNet/CNN consume. The learned
models are trained/exported separately (see ``train.py``); this module is the
shared, framework-free (numpy/librosa) front end used by BOTH training and
inference so the two never drift.

Channels (all mel-projected to ``N_MELS`` bands so they stack into one tensor):

  0. log-mel magnitude           — overall spectral envelope
  1. harmonic component (HPSS)    — tonal structure
  2. percussive component (HPSS)  — transient structure
  3. codec residual               — detail above a frequency-smoothed floor
     (what a lossy neural codec's overlap-add leaves behind)
  4. spectral-flatness map        — per-frame noise-likeness, broadcast to bands
  5. HF-emphasis envelope         — high-band energy over time (aliasing/ghosting)
  6. spectral flux                — frame-to-frame change (modulation artifacts)

Output shape: ``(N_CHANNELS, N_MELS, n_frames)`` float32, finite, deterministic.
"""
from __future__ import annotations

import numpy as np

N_CHANNELS = 7
N_MELS = 128
_N_FFT = 2048
_HOP = 512
_SR = 22050   # the rate features are defined at; callers should resample to this


def _mel_project(spec: np.ndarray, sr: int, n_fft: int) -> np.ndarray:
    """Project a linear-frequency spectrogram onto N_MELS mel bands."""
    import librosa

    mel_fb = librosa.filters.mel(sr=sr, n_fft=n_fft, n_mels=N_MELS)
    return mel_fb @ spec


def extract_forensic_features(y: np.ndarray, sr: int) -> np.ndarray:
    """Return the (N_CHANNELS, N_MELS, n_frames) forensic feature tensor."""
    import librosa
    from scipy.ndimage import median_filter

    y = np.asarray(y, dtype=np.float32)
    if sr != _SR and y.size > 0:
        y = librosa.resample(y, orig_sr=sr, target_sr=_SR)
    sr = _SR
    if y.size < _N_FFT:
        y = np.pad(y, (0, _N_FFT - y.size))

    S = np.abs(librosa.stft(y, n_fft=_N_FFT, hop_length=_HOP)) + 1e-9
    n_frames = S.shape[1]

    # HPSS on the magnitude spectrogram.
    from khensu_services.analysis._shared_analysis import hpss  # kernel-clamped (short clips)
    H, P = hpss(S)

    # Codec residual: detail above a frequency-smoothed floor.
    smooth = median_filter(S, size=(9, 1))
    residual = np.maximum(S - smooth, 0.0) + 1e-9

    # Per-frame spectral flatness, broadcast across the band axis.
    flat = librosa.feature.spectral_flatness(S=S)                    # (1, frames)
    flat_map = np.repeat(flat, N_MELS, axis=0)                       # (N_MELS, frames)

    # HF-emphasis: energy above ~6 kHz as a per-frame envelope, broadcast.
    freqs = librosa.fft_frequencies(sr=sr, n_fft=_N_FFT)
    hf = freqs >= 6000.0
    hf_env = (S[hf] ** 2).sum(axis=0, keepdims=True)                 # (1, frames)
    hf_env = hf_env / (hf_env.max() + 1e-9)
    hf_map = np.repeat(hf_env, N_MELS, axis=0)

    # Spectral flux (frame-to-frame magnitude change), broadcast.
    flux = np.zeros((1, n_frames), dtype=np.float32)
    if n_frames > 1:
        flux[0, 1:] = np.sqrt(np.mean(np.diff(S, axis=1) ** 2, axis=0))
    flux = flux / (flux.max() + 1e-9)
    flux_map = np.repeat(flux, N_MELS, axis=0)

    logmel = librosa.amplitude_to_db(_mel_project(S, sr, _N_FFT), ref=np.max)
    harm   = librosa.amplitude_to_db(_mel_project(H, sr, _N_FFT), ref=np.max)
    perc   = librosa.amplitude_to_db(_mel_project(P, sr, _N_FFT), ref=np.max)
    resid  = librosa.amplitude_to_db(_mel_project(residual, sr, _N_FFT), ref=np.max)

    channels = [
        _norm(logmel),
        _norm(harm),
        _norm(perc),
        _norm(resid),
        flat_map.astype(np.float32),
        hf_map.astype(np.float32),
        flux_map.astype(np.float32),
    ]
    feats = np.stack(channels, axis=0).astype(np.float32)
    return np.nan_to_num(feats, nan=0.0, posinf=0.0, neginf=0.0)


def _norm(db: np.ndarray) -> np.ndarray:
    """Map a dB channel (typically [-80, 0]) into [0, 1]."""
    return np.clip((db + 80.0) / 80.0, 0.0, 1.0).astype(np.float32)
