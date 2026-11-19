from __future__ import annotations

import numpy as np
import librosa


def score_heaviness(y: np.ndarray, sr: int) -> dict:
    D = np.abs(librosa.stft(y))
    freqs = librosa.fft_frequencies(sr=sr)

    total_energy = float(np.sum(D ** 2) + 1e-12)

    # Weight: sub-bass fraction, rescaled so 8% sub-bass → 1.0.
    # Raw fraction is typically 0.01–0.12 for most music; ×12 spreads it across
    # the full 0..1 radar axis without clipping heavy content.
    subbass_mask   = freqs <= 80.0
    subbass_energy = float(np.sum(D[subbass_mask] ** 2))
    weight = float(np.clip(subbass_energy / total_energy * 12.0, 0.0, 1.0))

    # Distortion: spectral flux in 2–8 kHz band normalised by total-band RMS.
    # Flux/RMS ratio for clean sine-waves is near 0; saturated distortion raises it.
    # Rescaled so a ratio of 0.5 → 1.0 (high distortion).
    high_mask = (freqs >= 2000.0) & (freqs <= 8000.0)
    D_high = D[high_mask]
    if D_high.shape[0] > 0 and D_high.shape[1] > 1:
        flux      = float(np.mean(np.abs(np.diff(D_high, axis=1))))
        rms_high  = float(np.sqrt(np.mean(D_high ** 2)) + 1e-12)
        distortion = float(np.clip(flux / rms_high * 2.0, 0.0, 1.0))
    else:
        distortion = 0.0

    # Clarity: fraction of (clar band) energy in (mud + clar) combined.
    # 0 = all mud (boxy/heavy), 1 = all clar (bright/clear).
    # Previous formula (1 - mud/clar) clipped to 0 whenever mud > clar, which is
    # true for virtually every track with a strong low-mid presence.
    mud_mask    = (freqs >= 200.0) & (freqs <= 500.0)
    clar_mask   = (freqs >= 1000.0) & (freqs <= 4000.0)
    mud_energy  = float(np.sum(D[mud_mask]  ** 2))
    clar_energy = float(np.sum(D[clar_mask] ** 2))
    clarity = float(np.clip(clar_energy / max(clar_energy + mud_energy, 1e-12), 0.0, 1.0))

    extremity = float((distortion + weight) / 2.0)

    return {
        "heaviness_distortion": distortion,
        "heaviness_weight":     weight,
        "heaviness_clarity":    clarity,
        "heaviness_extremity":  extremity,
    }
