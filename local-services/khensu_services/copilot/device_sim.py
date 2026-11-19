# local-services/khensu_services/copilot/device_sim.py
"""Translation Check device simulations: renders the mix as playback
devices actually present it, so "check it on your phone" becomes a button
instead of an export-and-walk-to-the-car ritual.

Each profile is a deliberately simple acoustic caricature — bandwidth
limits, mono folds, and characteristic EQ — tuned to expose the classic
translation failures (bass vanishing on phones, vocals drowning in cars,
mono-collapse kills on clubs), not to model any specific product.
"""
from __future__ import annotations

import os
from typing import Any

import numpy as np
import soundfile as sf
from pedalboard import Pedalboard, PeakFilter


# Each profile: mono fold, binaural flag, note, and a stage list. A stage is
# ("hp"|"lp", cutoff_hz, slope_db_per_oct) for a Butterworth filter (the same
# dsp.butterworth_filter the render chain uses, audit CPL-05) or
# ("peak", centre_hz, gain_db, q) for a pedalboard bell. pedalboard's own
# high/low-pass filters are first-order (6 dB/oct), far gentler than any real
# small-speaker roll-off; the old phone profile stacked two of them to fake
# 12 dB/oct. The unused per-profile ``board`` lambdas duplicated
# _device_board and drifted from it, so they are gone (§6.3).
_PROFILES: dict[str, dict[str, Any]] = {
    "phone": {
        # Tiny single speaker: nothing below ~300 Hz, presence bump, mono.
        "mono": True,
        "stages": [("hp", 300.0, 24.0), ("lp", 8000.0, 12.0), ("peak", 2500.0, 3.0, 1.0)],
        "note": "No bass exists here — if the groove depends on sub, this is where you find out.",
    },
    "earbuds": {
        # Sealed IEM-ish: hyped lows and highs, scooped mids. Binaural HRTF
        # folds the stereo image to the in-head headphone presentation.
        "mono": False,
        "binaural": True,
        "stages": [("peak", 80.0, 4.0, 0.8), ("peak", 800.0, -2.0, 0.9), ("peak", 9000.0, 3.0, 0.8)],
        "note": "Consumer-tuned smile curve, binaural (in-head) image — harshness, "
                "sibilance, and over-wide/anti-phase content jump out here first.",
    },
    "car": {
        # Cabin: big low boost, absorbed highs, engine-noise masking of quiet detail.
        "mono": False,
        "stages": [("peak", 60.0, 5.0, 0.7), ("peak", 1000.0, -1.5, 0.7), ("lp", 12000.0, 12.0)],
        "note": "Bass-heavy and midrange-masked — vocals that were 'fine' get buried here.",
    },
    "club": {
        # Big PA, mono sub, limited extreme top.
        "mono": True,
        "stages": [("peak", 50.0, 4.0, 0.7), ("lp", 14000.0, 12.0)],
        "note": "Mono sub + mono-summed PA: anti-phase width disappears entirely.",
    },
}


def _safe_frequency(hz: float, sample_rate: int) -> float:
    """Keep filter frequencies safely below source Nyquist."""
    return min(hz, sample_rate * 0.45)


def _apply_device_stages(device: str, audio_ch: np.ndarray, sample_rate: int) -> np.ndarray:
    """Run ``device``'s stage list over a (channels, frames) buffer."""
    from khensu_services.copilot.dsp import butterworth_filter

    out = np.asarray(audio_ch, dtype=np.float64)
    for stage in _PROFILES[device]["stages"]:
        kind = stage[0]
        if kind in ("hp", "lp"):
            out = butterworth_filter(out, sample_rate, _safe_frequency(stage[1], sample_rate),
                                     "high" if kind == "hp" else "low", stage[2])
        else:
            board = Pedalboard([PeakFilter(cutoff_frequency_hz=_safe_frequency(stage[1], sample_rate),
                                           gain_db=stage[2], q=stage[3])])
            out = board(out.astype(np.float32), sample_rate).astype(np.float64)
    return out


def list_device_profiles() -> list[dict[str, str]]:
    return [{"device": name, "note": p["note"]} for name, p in _PROFILES.items()]


def is_device(device: str) -> bool:
    return device in _PROFILES


def apply_device_profile(y: np.ndarray, sr: int, device: str) -> np.ndarray:
    """``device``'s playback chain over a ``(frames, channels)`` buffer: mono
    fold, filter stages, and the binaural fold for headphone-class devices.
    No output normalisation (the stream preview measures the result's level)."""
    if device not in _PROFILES:
        raise ValueError(f"Unknown device profile: {device}")
    profile = _PROFILES[device]
    y = np.asarray(y, dtype=np.float32)
    if profile["mono"] and y.shape[1] > 1:
        mono = y.mean(axis=1, keepdims=True)
        y = np.repeat(mono, y.shape[1], axis=1)

    out = _apply_device_stages(device, y.T, int(sr)).T   # stages take (ch, frames)
    if not np.isfinite(out).all():
        raise ValueError(f"Device simulation produced non-finite audio for {device}")

    # Binaural HRTF for headphone-class devices (earbuds): fold the stereo
    # image to the in-head presentation via head-related crossfeed. (The
    # binaural render scales a render peaking over 0.999 down to it.)
    if profile.get("binaural") and out.shape[1] >= 2:
        from khensu_services.copilot.hrtf import binaural_render
        out = binaural_render(out[:, :2], sr)
    return np.asarray(out, dtype=np.float64)


def render_device_preview(file_path: str, device: str) -> dict[str, Any]:
    if device not in _PROFILES:
        raise ValueError(f"Unknown device profile: {device}")
    profile = _PROFILES[device]

    y, sr = sf.read(file_path, dtype="float32", always_2d=True)  # (frames, ch)
    out = apply_device_profile(y, int(sr), device)

    peak = float(np.max(np.abs(out)) + 1e-12)
    if peak > 0.999:
        out = out * (0.999 / peak)

    base, _ = os.path.splitext(file_path)
    rendered = f"{base}_device_{device}.wav"
    # 24-bit PCM, atomic (soundfile's .wav default is undithered 16-bit).
    from khensu_services.copilot.dsp import _atomic_render_write
    _atomic_render_write(rendered, lambda tmp: sf.write(tmp, out, sr, subtype="PCM_24"))
    return {"device": device, "rendered_path": rendered, "note": profile["note"]}
