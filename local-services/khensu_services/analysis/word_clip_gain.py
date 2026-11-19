# local-services/khensu_services/analysis/word_clip_gain.py
"""Per-word clip gain: the surgical, manual counterpart to the Smart Gain
Rider. The user marks individual words/syllables and dials a static gain per
region; this renders the result.

Envelope is built in dB (so overlapping regions sum like stacked clip-gain
lanes), each region clamped to ±24 dB and the total clamped to ±24 dB, with
linear-in-dB ramps of ``fade_ms`` *inside* each region's edges so a word edit
never bleeds into its neighbour. No normalisation — the caller sees the true
output peak and decides. Written as float WAV so boosts above 0 dBFS are
preserved rather than clipped.
"""
from __future__ import annotations

import math
from typing import Any

import numpy as np

MAX_GAIN_DB = 24.0


def build_gain_envelope_db(n: int, sr: int, regions: list[dict[str, float]],
                           fade_ms: float) -> tuple[np.ndarray, int]:
    """Return (per-sample dB envelope, regions applied)."""
    env = np.zeros(n)
    fade_req = max(0, int(round(max(0.0, float(fade_ms)) * 1e-3 * sr)))
    duration = n / sr if sr else 0.0
    applied = 0
    for r in regions:
        start, end, gain = float(r["start_sec"]), float(r["end_sec"]), float(r["gain_db"])
        if not (math.isfinite(start) and math.isfinite(end) and math.isfinite(gain)):
            continue
        start, end = max(0.0, start), min(duration, end)
        a, b = int(round(start * sr)), int(round(end * sr))
        if b - a < 1:
            continue
        gain = float(np.clip(gain, -MAX_GAIN_DB, MAX_GAIN_DB))
        seg = np.full(b - a, gain)
        fade = min(fade_req, (b - a) // 2)
        if fade > 0:
            ramp = np.linspace(0.0, 1.0, fade + 1)[1:]
            seg[:fade] *= ramp
            seg[-fade:] *= ramp[::-1]
        env[a:b] += seg
        applied += 1
    np.clip(env, -MAX_GAIN_DB, MAX_GAIN_DB, out=env)
    return env, applied


def render_word_clip_gain(file_path: str, regions: list[dict[str, float]],
                          fade_ms: float = 8.0,
                          output_path: str | None = None) -> dict[str, Any]:
    import os

    import soundfile as sf
    from khensu_services.copilot.dsp import _atomic_render_write, require_finite_audio

    data, sr = sf.read(file_path, always_2d=True, dtype="float64")
    require_finite_audio(data, "word-clip-gain source")
    sr = int(sr)
    env_db, applied = build_gain_envelope_db(data.shape[0], sr, regions, fade_ms)
    out_audio = data * (10.0 ** (env_db / 20.0))[:, np.newaxis]

    peak = float(np.max(np.abs(out_audio))) if out_audio.size else 0.0
    peak_dbfs = 20.0 * math.log10(peak) if peak > 1e-6 else -120.0

    out = output_path or os.path.splitext(file_path)[0] + "_wordgain.wav"
    _atomic_render_write(out, lambda tmp: sf.write(tmp, out_audio, sr,
                                                   format="WAV", subtype="FLOAT"))
    return {"rendered_path": out, "regions_applied": applied,
            "peak_dbfs": round(peak_dbfs, 2)}
