"""Headphone-vs-speaker crossfeed A/B.

Plain headphones present a stereo mix "in head": each ear hears exactly one
channel with no acoustic crosstalk, so hard-panned elements sit unnaturally
wide and far to the side. Speakers (and headphone crossfeed) let each ear hear
both channels — the contralateral one delayed and head-shadowed — which pulls
the image in toward a natural speaker soundstage.

This renders both for a direct A/B: the raw in-head headphone image and the
crossfed speaker image (via hrtf.binaural_render), and measures how much the
mix's stereo width collapses on speakers so an over-wide mix is easy to catch.
"""
from __future__ import annotations

import os
from typing import Any, Optional

import numpy as np
import soundfile as sf


def _width(y: np.ndarray) -> float:
    """Shared stereo width (0 mono .. 1 uncorrelated) for (frames, ch)."""
    from khensu_services.analysis.dsp_metrics import stereo_width_frames
    return stereo_width_frames(y)


def _corr(y: np.ndarray) -> float:
    if y.ndim < 2 or y.shape[1] < 2:
        return 1.0
    from khensu_services.analysis.dsp_metrics import stereo_correlation
    return stereo_correlation(y[:, 0], y[:, 1])


_PEAK_CEILING = 10.0 ** (-1.0 / 20.0)   # -1 dBFS, shared by both files


def _level_matched_pair(hp: np.ndarray, sp: np.ndarray, sr: int) -> tuple[np.ndarray, np.ndarray, float]:
    """Loudness-match the speaker render to the headphone one, then give both
    the SAME attenuation if either would exceed -1 dBFS (audit CPL-13).

    Previously the speaker file was peak-normalised inside binaural_render and
    the headphone file hard-clipped with ``np.clip``, which left a 1.8 LU bias
    in favour of the in-head file (louder reads as better) and distorted hot
    sources. Returns (headphone, speaker, gain applied to the speaker in dB).
    """
    from khensu_services.copilot.render_guardrails import loudness_match

    sp_matched, gain_db = loudness_match(hp, sr, sp, sr)
    peak = max(float(np.max(np.abs(hp))) if hp.size else 0.0,
               float(np.max(np.abs(sp_matched))) if sp_matched.size else 0.0)
    trim = _PEAK_CEILING / peak if peak > _PEAK_CEILING else 1.0
    return hp * trim, sp_matched * trim, gain_db


def render_crossfeed_ab(src_path: str, out_dir: Optional[str] = None) -> dict[str, Any]:
    """Render the in-head headphone image and the crossfed speaker image, and
    report the width collapse between them. Never raises on the DSP path."""
    from khensu_services.copilot.hrtf import binaural_render
    from khensu_services.copilot.dsp import require_finite_audio

    stereo, sr = sf.read(src_path, always_2d=True)
    require_finite_audio(stereo, "crossfeed source")  # audit CQ-01 (np.clip won't fix NaN)
    if stereo.shape[1] == 1:
        stereo = np.repeat(stereo, 2, axis=1)

    speaker = np.asarray(binaural_render(stereo, sr), dtype=np.float64)
    # Align lengths (binaural_render trims to input length already, but be safe).
    n = min(len(stereo), len(speaker))
    hp, sp = stereo[:n], speaker[:n]

    out_dir = out_dir or os.path.join(os.path.dirname(src_path), "crossfeed_ab")
    os.makedirs(out_dir, exist_ok=True)
    base = os.path.splitext(os.path.basename(src_path))[0]
    hp_path = os.path.join(out_dir, f"{base}_headphone_inhead.wav")
    sp_path = os.path.join(out_dir, f"{base}_speaker_crossfeed.wav")
    # 24-bit PCM, atomic (soundfile's .wav default is undithered 16-bit).
    from khensu_services.copilot.dsp import _atomic_render_write
    hp_out, sp_out, match_gain_db = _level_matched_pair(hp, sp, sr)
    _atomic_render_write(hp_path, lambda tmp: sf.write(tmp, hp_out, sr, subtype="PCM_24"))
    _atomic_render_write(sp_path, lambda tmp: sf.write(tmp, sp_out, sr, subtype="PCM_24"))

    w_hp, w_sp = _width(hp), _width(sp)
    width_reduction_pct = round((1.0 - (w_sp / w_hp)) * 100.0, 1) if w_hp > 1e-6 else 0.0

    return {
        "headphone_path": hp_path,     # A: in-head (plain headphones)
        "speaker_path": sp_path,       # B: crossfed speaker image
        "width_headphone": round(w_hp, 3),
        "width_speaker": round(w_sp, 3),
        "width_reduction_pct": width_reduction_pct,
        "correlation_headphone": round(_corr(hp), 3),
        "correlation_speaker": round(_corr(sp), 3),
        "speaker_match_gain_db": round(match_gain_db, 2),
        "note": (
            f"On speakers the stereo width narrows ~{width_reduction_pct}% versus the "
            "in-head headphone image — audition B to judge how the mix holds up "
            "on a real soundstage."),
    }
