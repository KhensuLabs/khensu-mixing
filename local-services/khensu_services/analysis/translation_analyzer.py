# local-services/khensu_services/analysis/translation_analyzer.py
"""
Translation prediction: how a mix's *balance* (not just its loudness) is
likely to degrade on real playback devices. extended_analyzer.py's existing
analyze_translation_risk() is a lightweight heuristic (correlation/balance
distance) used for the 5-meter TranslationPanel UI; this module goes deeper
for a subset of the same environments by actually applying an approximate
per-device frequency-response curve to the mix's measured band-energy
profile and re-running the masking-collision math from masking.py on the
coloured result, so the risk score reflects "does this device's response
make previously-separate frequency content start to mask" rather than only
a handful of hand-picked spectral/dynamics heuristics.

Stateless, numpy-only (mirrors extended_analyzer.py / masking.py's style).
"""
from __future__ import annotations
import numpy as np
import numpy.typing as npt
from typing import Any

from khensu_services.analysis.masking import _MASKING_BANDS, _collision_score


# --- 1/3-octave resolution (Gemini audit) -------------------------------------
# The device coloration used to be 10 coarse bands (imported from masking.py).
# It's now resolved to the 31 standard ISO 1/3-octave bands so a narrow
# presence bump or a sharp roll-off shapes the prediction where it actually
# happens instead of being smeared across a 2-octave bucket. The coarse
# _DEVICE_RESPONSE_DB below stays as the authored source curve; it's
# log-frequency-interpolated onto the 31 bands.
_THIRD_OCTAVE_CENTERS: list[float] = [
    20.0, 25.0, 31.5, 40.0, 50.0, 63.0, 80.0, 100.0, 125.0, 160.0,
    200.0, 250.0, 315.0, 400.0, 500.0, 630.0, 800.0, 1000.0, 1250.0, 1600.0,
    2000.0, 2500.0, 3150.0, 4000.0, 5000.0, 6300.0, 8000.0, 10000.0, 12500.0,
    16000.0, 20000.0,
]
_THIRD_OCTAVE_EDGE = 2.0 ** (1.0 / 6.0)   # center / edge .. center * edge
_THIRD_OCTAVE_BANDS: list[tuple[float, float]] = [
    (c / _THIRD_OCTAVE_EDGE, c * _THIRD_OCTAVE_EDGE) for c in _THIRD_OCTAVE_CENTERS
]

# Approximate per-band gain (dB) applied relative to a flat/studio reference,
# derived from published typical frequency-response measurements for each
# device class (roll-off below the driver's low-frequency limit, presence
# bumps, etc.) — a fixed lookup table, not a learned model. Order matches the
# coarse _MASKING_BANDS: 20-60, 60-120, 120-250, 250-500, 500-1k, 1k-2k, 2k-4k,
# 4k-8k, 8k-12k, 12k-20k Hz.
_DEVICE_RESPONSE_DB: dict[str, list[float]] = {
    # Earbuds: weak sub, bass-boosted low-mid (small driver + seal
    # resonance), forward presence, rolled-off air.
    "earbuds": [-8.0, -1.0, 2.0, 1.0, 0.0, 1.0, 3.0, 2.0, -1.0, -4.0],
    # Phone speaker: almost no bass below ~150Hz, strong upper-mid/presence
    # forwardness to compensate, harsh top before rolling off hard.
    "phone":   [-18.0, -12.0, -4.0, 1.0, 2.0, 4.0, 5.0, 3.0, -2.0, -8.0],
    # Car system: bass-heavy (cabin resonance + subwoofer), scooped mids,
    # smooth top.
    "car":     [3.0, 4.0, 1.0, -1.0, -1.0, 0.0, 1.0, 1.0, 0.0, -2.0],
    # Club PA: very heavy sub/bass (reinforced), slight mid dip, bright top
    # for cut-through over crowd noise.
    "club":    [5.0, 4.0, 2.0, -1.0, -2.0, 0.0, 2.0, 3.0, 2.0, 0.0],
}

# A collision-score increase below this (0..1 scale, same units as
# _collision_score) between the flat and device-coloured mix is treated as
# noise, not a real new masking risk.
_COLLISION_INCREASE_THRESHOLD = 0.08


def _third_octave_energy(mono: npt.NDArray, sr: int, n_fft: int = 8192) -> npt.NDArray:
    """Normalized (sums to 1) per-1/3-octave energy, averaged over the track
    via overlapping Hann-windowed FFTs. Finer n_fft than the 10-band profile so
    the low 1/3-octave bins are actually resolved.

    Energy is the frame-averaged POWER ``mean(|X|^2)`` summed per band (the
    shared ``dsp_metrics.averaged_power_spectrum``), not ``(mean|X|)^2``, which
    under-weighted bands whose level fluctuates (DSP-15)."""
    from khensu_services.analysis.dsp_metrics import averaged_power_spectrum

    # The nominal ISO centres are not exactly contiguous, so the bands are
    # summed by mask over the shared averaged power spectrum.
    power = averaged_power_spectrum(mono, n_fft)
    freqs = np.fft.rfftfreq(n_fft, d=1.0 / sr)
    energies = np.array([float(np.sum(power[(freqs >= lo) & (freqs < hi)]))
                         for lo, hi in _THIRD_OCTAVE_BANDS])
    total = float(energies.sum())
    return energies / total if total > 0.0 else energies


def _device_response_31(name: str) -> npt.NDArray:
    """Log-frequency interpolation of the authored 10-band device curve onto
    the 31 1/3-octave centers (dB)."""
    coarse_centers = np.array([np.sqrt(lo * hi) for lo, hi in _MASKING_BANDS])
    coarse_db = np.array(_DEVICE_RESPONSE_DB[name])
    centers = np.array(_THIRD_OCTAVE_CENTERS)
    return np.interp(np.log10(centers), np.log10(coarse_centers), coarse_db)


def _adjacent_pair_collisions(band_energy: npt.NDArray) -> list[float]:
    """_collision_score between each pair of spectrally-adjacent bands —
    adjacent bands are the ones a frequency-response curve can plausibly
    push into masking each other (curve reshapes relative levels, it can't
    make band 1 and band 9 suddenly collide)."""
    return [
        _collision_score(np.array([band_energy[i]]), np.array([band_energy[i + 1]]))
        for i in range(len(band_energy) - 1)
    ]


def _apply_device_response(flat_energy: npt.NDArray, gains_db: npt.NDArray) -> npt.NDArray:
    linear_gain = np.power(10.0, gains_db / 10.0)  # power-domain, energy not amplitude
    coloured = flat_energy * linear_gain
    total = float(coloured.sum())
    return coloured / total if total > 0.0 else coloured


def analyze_translation(y_mono: npt.NDArray, sr: int) -> dict[str, Any]:
    """Returns {"per_profile": {device: {...}}} — see TranslationProfileScore
    in models.py for the per-device shape. 1/3-octave resolution (31 bands)."""
    flat_energy = _third_octave_energy(y_mono, sr)
    flat_collisions = _adjacent_pair_collisions(flat_energy)

    per_profile: dict[str, Any] = {}
    for device in _DEVICE_RESPONSE_DB:
        coloured_energy = _apply_device_response(flat_energy, _device_response_31(device))
        coloured_collisions = _adjacent_pair_collisions(coloured_energy)

        new_collisions = []
        deltas = []
        for i, (base, new) in enumerate(zip(flat_collisions, coloured_collisions)):
            delta = new - base
            deltas.append(max(delta, 0.0))
            if delta >= _COLLISION_INCREASE_THRESHOLD:
                lo_a, hi_a = _THIRD_OCTAVE_BANDS[i]
                lo_b, hi_b = _THIRD_OCTAVE_BANDS[i + 1]
                new_collisions.append({
                    "band_a_hz": [round(lo_a, 1), round(hi_a, 1)],
                    "band_b_hz": [round(lo_b, 1), round(hi_b, 1)],
                    "collision_score": round(new, 3),
                    "increase": round(delta, 3),
                })

        # Degradation score: mean of the positive collision-score increases
        # across all adjacent-band pairs, 0..1 (near-0 = device response
        # barely changes what masks what, near-1 = the coloration collapses
        # most band distinctions).
        degradation = float(np.mean(deltas)) if deltas else 0.0

        per_profile[device] = {
            "degradation_score": round(degradation, 3),
            "new_masking_collisions": sorted(new_collisions, key=lambda c: -c["increase"]),
        }

    return {"per_profile": per_profile}
