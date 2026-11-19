# local-services/khensu_services/analysis/gain_rider.py
"""Smart Gain Rider: phrase-level clip-gain automation for vocals, computed
BEFORE compression ever sees the signal.

The classic vocal-chain insight: do the heavy dynamics work with gain
automation at the *start* of the chain so the compressor receives an
already-consistent level and can work musically (2-4 dB of gain reduction)
instead of fighting a 20 dB performance range. Loud phrases get pulled
down, quiet phrases pushed up, each toward the -18 dBFS RMS sweet spot.

Output is both data (phrase segments + a sample-rate-agnostic gain
envelope for UI overlay) and, optionally, a rendered file with the
envelope applied (short LINEAR gain ramps at phrase boundaries — no
zipper noise, no breath chopping).

Stateless, numpy-only analysis; soundfile for the optional render.
"""
from __future__ import annotations

import os
from typing import Any

import numpy as np
import numpy.typing as npt
from khensu_services.analysis.context import load_audio

_WIN_SEC = 0.050          # RMS analysis window
_GATE_OPEN_DB = 12.0      # phrase gate: this far above the noise floor
_MIN_PHRASE_SEC = 0.15    # shorter bursts merge into neighbors
_MERGE_GAP_SEC = 0.25     # gaps shorter than this join two phrases
_RAMP_SEC = 0.040         # gain ramp at phrase boundaries


def _window_rms_db(mono: npt.NDArray, sr: int) -> tuple[npt.NDArray, int]:
    win = max(1, int(_WIN_SEC * sr))
    n_win = len(mono) // win
    if n_win == 0:
        return np.array([-90.0]), win
    rms = np.sqrt(np.mean(mono[: n_win * win].reshape(n_win, win) ** 2, axis=1) + 1e-20)
    return 20.0 * np.log10(rms + 1e-12), win


def compute_gain_ride(file_path: str, target_rms_db: float = -18.0,
                      max_gain_db: float = 12.0,
                      render: bool = False,
                      output_path: str | None = None) -> dict[str, Any]:
    """Segment the vocal into phrases and compute per-phrase static gains
    toward target_rms_db. When render=True, also writes the ridden file -
    to ``output_path`` when given (the router validates it), else
    ``<file>_gainride.wav`` next to the source - and returns its path."""
    import soundfile as sf
    from khensu_services.copilot.dsp import require_finite_audio

    y, sr = load_audio(file_path, sr=None, mono=False)
    require_finite_audio(y, "gain-rider source")  # audit CQ-01
    sr = int(sr)
    mono = y.mean(axis=0) if y.ndim > 1 else y

    win_db, win = _window_rms_db(mono, sr)
    n_win = len(win_db)
    win_sec = win / sr

    # Noise floor and phrase gate.
    floor_db = float(np.percentile(win_db, 10))
    gate_db = floor_db + _GATE_OPEN_DB
    active = win_db > gate_db

    # Merge tiny gaps, drop tiny phrases.
    min_phrase = max(1, int(_MIN_PHRASE_SEC / win_sec))
    merge_gap = max(1, int(_MERGE_GAP_SEC / win_sec))

    segments: list[tuple[int, int]] = []   # [start_win, end_win)
    i = 0
    while i < n_win:
        if not active[i]:
            i += 1
            continue
        j = i
        gap = 0
        while j < n_win and gap <= merge_gap:
            if active[j]:
                gap = 0
            else:
                gap += 1
            j += 1
        end = j - gap
        if end - i >= min_phrase:
            segments.append((i, end))
        i = j

    phrases: list[dict[str, Any]] = []
    for (a, b) in segments:
        seg_db = win_db[a:b]
        # Phrase level = energy mean of the active windows (dB of mean power,
        # not mean of dBs, so loud moments weigh in properly).
        power = np.mean(10.0 ** (seg_db / 10.0))
        phrase_db = float(10.0 * np.log10(power + 1e-20))
        gain = float(np.clip(target_rms_db - phrase_db, -max_gain_db, max_gain_db))
        phrases.append({
            "start_sec": round(a * win_sec, 3),
            "end_sec": round(b * win_sec, 3),
            "rms_db": round(phrase_db, 2),
            "gain_db": round(gain, 2),
        })

    # Downsampled envelope for the UI overlay (one point per analysis window).
    env_db = np.zeros(n_win)
    for p, (a, b) in zip(phrases, segments):
        env_db[a:b] = p["gain_db"]

    result: dict[str, Any] = {
        "file_path": file_path,
        "target_rms_db": target_rms_db,
        "noise_floor_db": round(floor_db, 2),
        "window_sec": round(win_sec, 4),
        "phrases": phrases,
        "envelope_db": [round(float(v), 2) for v in env_db],
        "rendered_path": None,
    }

    if render and phrases:
        gain_lin = np.ones(len(mono))
        ramp = max(1, int(_RAMP_SEC * sr))
        for p, (a, b) in zip(phrases, segments):
            g = 10.0 ** (p["gain_db"] / 20.0)
            s0, s1 = a * win, min(b * win, len(mono))
            gain_lin[s0:s1] = g
            # Linear ramps in/out so boundaries don't click.
            r0 = max(0, s0 - ramp)
            gain_lin[r0:s0] = np.linspace(gain_lin[r0], g, s0 - r0, endpoint=False)
            r1 = min(len(mono), s1 + ramp)
            gain_lin[s1:r1] = np.linspace(g, 1.0, r1 - s1, endpoint=False)

        out = y * gain_lin if y.ndim == 1 else y * gain_lin[np.newaxis, :]
        # Safety: never render a file over -1 dBTP. AUDIT-082: measured as
        # true peak, since a sample-peak cap still lets inter-sample overs clip
        # on playback.
        from khensu_services.analysis.loudness import true_peak_linear
        ceiling = 10.0 ** (-1.0 / 20.0)
        peak = true_peak_linear(out)
        if peak > ceiling:
            out = out * (ceiling / peak)
            result["peak_limited_db"] = round(20.0 * np.log10(ceiling / peak), 2)

        if output_path:
            rendered = output_path
        else:
            base, _ = os.path.splitext(file_path)
            rendered = base + "_gainride.wav"
        # 24-bit PCM via the atomic render helper (soundfile's .wav default is
        # undithered 16-bit, which requantises every ride audibly on fades).
        from khensu_services.copilot.dsp import _atomic_render_write
        _atomic_render_write(rendered, lambda tmp: sf.write(
            tmp, out.T if out.ndim > 1 else out, sr, subtype="PCM_24"))
        result["rendered_path"] = rendered

    return result
