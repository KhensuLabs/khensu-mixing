# local-services/khensu_services/analysis/arrangement_analyzer.py
"""Arrangement Element Analyzer: classifies the separated stems into the
five classic arrangement elements (Owsinski) per time window and flags
gaps/overcrowding.

Elements and how they map onto the 4-stem separation:
  foundation — bass stem + the drums stem's low band (kick): the pulse the
               track stands on
  rhythm     — the drums stem's mid/high band (snare, hats, percussion)
  pad        — the sustained share of the "other" stem (chords, strings,
               synth beds)
  lead       — the vocals stem (plus nothing else: with 4 stems the lead
               instrument of an instrumental lives in "other", which the
               response notes when vocals are silent)
  fills      — the transient share of the "other" stem (stabs, ear candy,
               one-shot ornaments)

Per fixed window we report each element's share of total energy and derive
flags: "overcrowded" (too many strong elements at once), "no_foundation",
"no_lead". Stateless, numpy/librosa-only (mirrors extended_analyzer.py).
"""
from __future__ import annotations

from khensu_services.analysis.context import load_audio
from typing import Any

import numpy as np
import numpy.typing as npt
from khensu_services.analysis.dsp_metrics import onset_strength


def _band_split_energy(y: npt.NDArray, sr: int, split_hz: float,
                       window_samples: int) -> tuple[npt.NDArray, npt.NDArray]:
    """Per-window (low, high) band energy via single-pole-ish FFT masking of
    each window — cheap and adequate for energy shares."""
    n_win = len(y) // window_samples
    lows = np.zeros(max(n_win, 1))
    highs = np.zeros(max(n_win, 1))
    for i in range(n_win):
        seg = y[i * window_samples : (i + 1) * window_samples]
        # Parseval-normalized so these are commensurate with the time-domain
        # sums _windowed_energy() produces for the other stems.
        spec = np.abs(np.fft.rfft(seg)) ** 2 / max(len(seg), 1)
        freqs = np.fft.rfftfreq(len(seg), d=1.0 / sr)
        lows[i] = float(np.sum(spec[(freqs >= 20.0) & (freqs < split_hz)]))
        highs[i] = float(np.sum(spec[freqs >= split_hz]))
    return lows, highs


def _windowed_energy(y: npt.NDArray, window_samples: int) -> npt.NDArray:
    n_win = len(y) // window_samples
    if n_win == 0:
        return np.zeros(1)
    return np.array([
        float(np.sum(y[i * window_samples : (i + 1) * window_samples] ** 2))
        for i in range(n_win)
    ])


def _sustained_transient_split(y: npt.NDArray, sr: int,
                               window_samples: int) -> tuple[npt.NDArray, npt.NDArray]:
    """Split per-window energy of a stem into sustained (pad-like) vs
    transient (fill-like) shares using spectral-flux-weighted framing:
    windows where energy arrives in short bursts count as transient."""

    hop = 512
    onset_env = onset_strength(y, sr, hop_length=hop)
    frames_per_window = max(1, window_samples // hop)
    n_win = len(y) // window_samples

    total = _windowed_energy(y, window_samples)
    sustained = np.zeros(max(n_win, 1))
    transient = np.zeros(max(n_win, 1))
    for i in range(n_win):
        seg_flux = onset_env[i * frames_per_window : (i + 1) * frames_per_window]
        if seg_flux.size == 0:
            sustained[i] = total[i]
            continue
        # Burstiness: how top-heavy the flux distribution is. All-flux-in-
        # few-frames => transient material; flat flux => sustained.
        flux_sum = float(np.sum(seg_flux)) + 1e-12
        top_share = float(np.sum(np.sort(seg_flux)[-max(1, len(seg_flux) // 8):])) / flux_sum
        transient_ratio = float(np.clip((top_share - 0.3) / 0.5, 0.0, 1.0))
        transient[i] = total[i] * transient_ratio
        sustained[i] = total[i] * (1.0 - transient_ratio)
    return sustained, transient


def analyze_arrangement(vocals_path: str, drums_path: str, bass_path: str,
                        other_path: str, window_sec: float = 8.0) -> dict[str, Any]:
    """Per-window five-element energy shares + arrangement flags."""

    stems: dict[str, tuple[npt.NDArray, int]] = {}
    for name, path in (("vocals", vocals_path), ("drums", drums_path),
                       ("bass", bass_path), ("other", other_path)):
        y, sr = load_audio(path, sr=None, mono=True)
        stems[name] = (y, int(sr))

    sr = stems["vocals"][1]
    window_samples = int(window_sec * sr)
    n_win = min(len(y) // window_samples for (y, _) in stems.values())
    if n_win == 0:
        return {"window_sec": window_sec, "windows": [], "flags": [],
                "element_presence": {}}

    drums_low, drums_high = _band_split_energy(stems["drums"][0], sr, 150.0, window_samples)
    bass_energy = _windowed_energy(stems["bass"][0], window_samples)
    vocal_energy = _windowed_energy(stems["vocals"][0], window_samples)
    other_sustained, other_transient = _sustained_transient_split(
        stems["other"][0], sr, window_samples)

    windows: list[dict[str, Any]] = []
    flags: list[dict[str, Any]] = []
    presence_count = {"foundation": 0, "rhythm": 0, "pad": 0, "lead": 0, "fills": 0}

    for i in range(n_win):
        elements = {
            "foundation": float(bass_energy[i] + drums_low[i]),
            "rhythm":     float(drums_high[i]),
            "pad":        float(other_sustained[i]),
            "lead":       float(vocal_energy[i]),
            "fills":      float(other_transient[i]),
        }
        total = sum(elements.values()) + 1e-12
        shares = {k: v / total for k, v in elements.items()}

        # An element is "present" when it carries a meaningful share and the
        # window itself isn't silence.
        window_db = 10.0 * np.log10(total + 1e-12)
        silent = window_db < -70.0
        present = {} if silent else {k: v > 0.08 for k, v in shares.items()}
        strong = [k for k, v in shares.items() if not silent and v > 0.22]

        for k, p in present.items():
            if p:
                presence_count[k] += 1

        start = i * window_sec
        w = {
            "start_sec": start,
            "end_sec": start + window_sec,
            "shares": {k: round(v, 4) for k, v in shares.items()},
            "present": present,
            "silent": silent,
        }
        windows.append(w)

        if silent:
            continue
        if len(strong) >= 4:
            flags.append({
                "start_sec": start, "end_sec": start + window_sec,
                "code": "overcrowded",
                "message": (f"{len(strong)} elements competing at full strength "
                            f"({', '.join(strong)}) — consider thinning; every element "
                            "fighting for attention means none of them wins."),
            })
        if shares["foundation"] < 0.04 and shares["rhythm"] > 0.15:
            flags.append({
                "start_sec": start, "end_sec": start + window_sec,
                "code": "no_foundation",
                "message": "Rhythm without foundation — the groove has no low-end anchor here.",
            })
        if shares["lead"] < 0.03 and (shares["pad"] + shares["fills"]) > 0.4:
            flags.append({
                "start_sec": start, "end_sec": start + window_sec,
                "code": "no_lead",
                "message": ("No lead element carrying the ear (vocals silent; if an "
                            "instrument leads here it's buried in the 'other' stem)."),
            })

    return {
        "window_sec": window_sec,
        "windows": windows,
        "flags": flags,
        "element_presence": {
            k: round(v / n_win, 3) for k, v in presence_count.items()
        },
    }
