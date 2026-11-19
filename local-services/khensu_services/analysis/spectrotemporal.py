# local-services/khensu_services/analysis/spectrotemporal.py
"""
Spectro-temporal heatmap export: a downsampled log-frequency-band x
time-frame magnitude grid, for the desktop Dashboard's "Timeline" tab.

Existing analyzers (extended_analyzer.py, masking.py) all average FFTs over
the whole track or per-stem; none of them export a genuinely time-varying
spectrogram. This is the "backend already computes FFTs, just needs a
downsampled frame export endpoint" item from the 2026-07-07 audit.

Stateless, numpy/librosa-only (mirrors the other analyzer modules' style).
"""
from __future__ import annotations
import numpy as np
import librosa
from typing import Any

# Heatmap dynamic range below the loudest cell. Everything quieter than this
# reads as the bottom colour, so a near-silent cell (or a fade to digital
# silence) can't stretch the scale and wash the music into one colour.
_DISPLAY_RANGE_DB = 80.0
_N_FFT = 2048


def compute_spectrotemporal_frames(path: str, n_bands: int = 24,
                                    target_frames: int = 200) -> dict[str, Any]:
    """Log-frequency-spaced (20 Hz..Nyquist) x time-downsampled STFT magnitude
    grid, normalized to 0-1 (dB-scaled) for direct use as heatmap colour
    intensity. n_bands/target_frames are deliberately coarse — this feeds a
    UI heatmap lane, not a precision spectral analysis.
    """
    from khensu_services.analysis.context import AnalysisContext

    y, sr = AnalysisContext(path).load(sr=None, mono=True)   # shared decode cache
    duration = len(y) / sr if sr > 0 else 0.0

    n_fft = _N_FFT
    # hop <= window so every sample is analysed (AUDIT-083): a hop of
    # len/target_frames (~44k samples on a 3-minute track) against a 2048
    # window skipped ~95% of the audio. Extra frames are averaged per output
    # column below.
    hop = max(1, min(n_fft, len(y) // max(1, target_frames)))
    stft = np.abs(librosa.stft(y, n_fft=n_fft, hop_length=hop))
    freqs = librosa.fft_frequencies(sr=sr, n_fft=n_fft)

    nyquist = sr / 2.0
    hi_hz = min(20000.0, nyquist) if nyquist > 20.0 else nyquist
    band_edges = np.logspace(np.log10(20.0), np.log10(max(hi_hz, 21.0)), n_bands + 1)

    n_stft_frames = stft.shape[1]
    n_out_frames = max(1, min(target_frames, n_stft_frames))
    # Column k averages the STFT frames in [bounds[k], bounds[k+1]): bounds
    # proportional to time, so the column axis is linear in time. (array_split
    # front-loads the remainder, putting a mid-track event ~1-2 % of the
    # duration away from its nominal column.)
    bounds = (np.arange(n_out_frames + 1) * n_stft_frames) // n_out_frames
    frame_groups = [np.arange(bounds[k], bounds[k + 1]) for k in range(n_out_frames)]

    grid = np.zeros((n_bands, n_out_frames))
    for band_idx in range(n_bands):
        lo, hi = band_edges[band_idx], band_edges[band_idx + 1]
        band_mask = (freqs >= lo) & (freqs < hi)
        if band_mask.any():
            band_energy = stft[band_mask, :].mean(axis=0)
        else:
            # A low band narrower than one FFT bin holds no bin; read the bin
            # nearest its geometric centre instead of a fake -180 dB floor.
            nearest = int(np.argmin(np.abs(freqs - np.sqrt(lo * hi))))
            band_energy = stft[nearest, :]
        for frame_idx, group in enumerate(frame_groups):
            grid[band_idx, frame_idx] = float(band_energy[group].mean()) if len(group) else 0.0

    # dB-scale then min-max normalize so quiet passages don't all read as 0
    # and the heatmap uses its full colour range regardless of the track's
    # absolute loudness.
    grid_db = 20.0 * np.log10(grid + 1e-9)
    hi_db = float(grid_db.max())
    lo_db = max(float(grid_db.min()), hi_db - _DISPLAY_RANGE_DB)
    rng_db = hi_db - lo_db if hi_db > lo_db else 1.0
    grid_norm = np.clip((grid_db - lo_db) / rng_db, 0.0, 1.0)

    return {
        "duration_sec": duration,
        "n_bands": n_bands,
        "band_edges_hz": band_edges.tolist(),
        "n_frames": n_out_frames,
        # [frame][band] = 0..1 normalized magnitude, row-major by time so the
        # desktop side can iterate frames in playback order without a transpose.
        "frames": grid_norm.T.tolist(),
    }
