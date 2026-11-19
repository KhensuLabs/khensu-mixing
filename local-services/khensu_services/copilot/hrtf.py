# local-services/khensu_services/copilot/hrtf.py
"""Lightweight synthetic HRTF for binaural headphone modeling (Gemini audit).

Real HRIR databases (SOFA/CIPIC) are large binary assets we deliberately don't
vendor. Instead we synthesize a compact spherical-head HRIR pair procedurally:

  * ITD  — Woodworth's spherical-head inter-aural time difference.
  * ILD / head shadow — the contralateral ear is delayed, broadband-attenuated,
    and low-passed (the head blocks highs more than lows).

`binaural_render()` uses one ±azimuth HRIR pair to fold a stereo mix down to
what each ear actually receives on headphones — the two channels become two
virtual speakers with crossfeed and head-related filtering, so the earbud
"Hear it" preview reflects binaural (in-head) presentation rather than a raw
hard-panned stereo feed.

A baked 44.1 kHz asset (assets/hrir_synth.npz) is loaded when present;
otherwise the IRs are synthesized at the track's sample rate on the fly.
"""
from __future__ import annotations

import os
from typing import Any

import numpy as np
import numpy.typing as npt
import scipy.signal as sig

_HEAD_RADIUS_M = 0.0875   # ~mean human head radius
_SPEED_OF_SOUND = 343.0
_DEFAULT_AZIMUTH = 30.0   # virtual-speaker half-angle
_ASSET_SR = 44100
_ASSET_PATH = os.path.join(os.path.dirname(__file__), "assets", "hrir_synth.npz")


def synth_hrir(azimuth_deg: float, sr: int, n_taps: int = 256) -> tuple[npt.NDArray, npt.NDArray]:
    """Ipsilateral and contralateral ear impulse responses for a source at
    `azimuth_deg` off-centre. Returns (ir_ipsi, ir_contra), each length n_taps.
    """
    az = np.radians(abs(azimuth_deg))

    # Woodworth ITD: contra ear lags the ipsi ear by this much.
    itd_sec = (_HEAD_RADIUS_M / _SPEED_OF_SOUND) * (az + np.sin(az))
    d_contra = int(round(itd_sec * sr))
    d_ipsi = 1  # tiny common delay so both IRs are causal

    # Head shadow on the contra ear: broadband ILD + a low-pass that closes
    # down as the source moves to the side.
    ild_db = -(abs(azimuth_deg) / 90.0) * 3.5
    fc = max(700.0, 6000.0 * (1.0 - 0.7 * (abs(azimuth_deg) / 90.0)))

    ir_ipsi = np.zeros(n_taps, dtype=np.float64)
    ir_contra = np.zeros(n_taps, dtype=np.float64)
    if d_ipsi < n_taps:
        ir_ipsi[d_ipsi] = 1.0
    if d_contra + d_ipsi < n_taps:
        ir_contra[d_contra + d_ipsi] = 10.0 ** (ild_db / 20.0)

    # One-pole low-pass the contra ear (head shadow).
    alpha = 1.0 - np.exp(-2.0 * np.pi * fc / sr)
    ir_contra = np.asarray(sig.lfilter([alpha], [1.0, -(1.0 - alpha)], ir_contra), dtype=np.float64)

    return ir_ipsi.astype(np.float64), ir_contra


def load_hrir(sr: int, azimuth_deg: float = _DEFAULT_AZIMUTH) -> tuple[npt.NDArray, npt.NDArray]:
    """The baked asset when it matches (sr, azimuth), else a fresh synth."""
    if sr == _ASSET_SR and abs(azimuth_deg - _DEFAULT_AZIMUTH) < 1e-6 and os.path.isfile(_ASSET_PATH):
        data = np.load(_ASSET_PATH)
        return np.asarray(data["ir_ipsi"], dtype=np.float64), np.asarray(data["ir_contra"], dtype=np.float64)
    return synth_hrir(azimuth_deg, sr)


def binaural_render(stereo: npt.NDArray, sr: int, azimuth_deg: float = _DEFAULT_AZIMUTH) -> npt.NDArray:
    """Fold a (frames, 2) stereo mix to a binaural (frames, 2) headphone image.

    The two channels become virtual speakers at -/+azimuth; each ear sums the
    ipsilateral speaker (near HRIR) and the contralateral one (delayed, shadowed
    HRIR) — i.e. head-related crossfeed.
    """
    if stereo.ndim == 1:
        stereo = np.column_stack([stereo, stereo])
    if stereo.shape[1] == 1:
        stereo = np.repeat(stereo, 2, axis=1)

    left_in = stereo[:, 0]
    right_in = stereo[:, 1]
    ir_ipsi, ir_contra = load_hrir(sr, azimuth_deg)

    def _c(x: npt.NDArray, h: npt.NDArray) -> npt.NDArray:
        return sig.fftconvolve(x, h)[: len(x)]

    left_ear = _c(left_in, ir_ipsi) + _c(right_in, ir_contra)
    right_ear = _c(right_in, ir_ipsi) + _c(left_in, ir_contra)

    out = np.column_stack([left_ear, right_ear])
    peak = float(np.max(np.abs(out)) + 1e-12)
    if peak > 0.999:
        out = out * (0.999 / peak)
    return out.astype(np.float32)


def lateral_angle_deg(azimuth_deg: float, elevation_deg: float = 0.0) -> float:
    """Interaural-polar lateral angle of a source (degrees, -90..90, positive =
    left with the ADM / BS.2051 convention that +azimuth is to the left).

    A spherical head hears only this angle: ``asin(sin(az) * cos(el))``. A
    height speaker at 45 deg elevation and 30 deg azimuth is therefore closer
    to the median plane than its bed speaker below it, and front / back
    sources at the same lateral angle are indistinguishable (the cone of
    confusion - this synthetic model has no pinna)."""
    az, el = np.radians(azimuth_deg), np.radians(elevation_deg)
    return float(np.degrees(np.arcsin(np.clip(np.sin(az) * np.cos(el), -1.0, 1.0))))


# Rear speakers lose top end at the ear (pinna / head shadow from behind);
# the synthetic head has no pinna, so a gentle one-pole low-pass on sources
# behind the listener (|azimuth| > 90 deg) stands in for it.
_REAR_SHADOW_FC = 9000.0


def speaker_ear_irs(azimuth_deg: float, elevation_deg: float, sr: int,
                    n_taps: int = 256) -> tuple[npt.NDArray, npt.NDArray]:
    """``(ir_left_ear, ir_right_ear)`` for one virtual speaker.

    Uses the same Woodworth / head-shadow synth as ``synth_hrir`` at the
    speaker's lateral angle (``lateral_angle_deg``); a source on the left
    feeds the left ear through the ipsilateral IR and the right ear through
    the contralateral one, and vice versa. A median-plane source (lateral 0)
    reaches both ears identically. Sources behind the head get the rear
    shadow low-pass on both ears."""
    lat = lateral_angle_deg(azimuth_deg, elevation_deg)
    ipsi, contra = synth_hrir(lat, sr, n_taps)
    if abs(lat) < 1e-6:
        left, right = ipsi, ipsi.copy()      # median plane: no ITD, no shadow
    else:
        left, right = (ipsi, contra) if lat > 0.0 else (contra, ipsi)
    if abs(((azimuth_deg + 180.0) % 360.0) - 180.0) > 90.0:
        alpha = 1.0 - np.exp(-2.0 * np.pi * min(_REAR_SHADOW_FC, 0.45 * sr) / sr)
        left = np.asarray(sig.lfilter([alpha], [1.0, -(1.0 - alpha)], left), dtype=np.float64)
        right = np.asarray(sig.lfilter([alpha], [1.0, -(1.0 - alpha)], right), dtype=np.float64)
    return np.asarray(left, dtype=np.float64), np.asarray(right, dtype=np.float64)


def binaural_render_speakers(signals: npt.NDArray, positions: list[tuple[float, float]],
                             sr: int, gains: list[float] | None = None) -> npt.NDArray:
    """Binaural ``(2, frames)`` render of a speaker layout.

    ``signals`` is ``(channels, frames)``; ``positions`` gives each channel's
    ``(azimuth_deg, elevation_deg)`` (ADM convention, +azimuth = left);
    ``gains`` (default 1.0) scales each speaker feed (0 drops it - e.g. an
    LFE the caller wants excluded). No peak normalisation: the caller
    measures the render's loudness against the source, so its level must be
    the render's own."""
    x = np.asarray(signals)
    if x.ndim == 1:
        x = x[np.newaxis, :]
    if len(positions) != x.shape[0]:
        raise ValueError("binaural_render_speakers: one position per channel is required")
    out = np.zeros((2, x.shape[1]), dtype=np.float64)
    for c in range(x.shape[0]):
        g = 1.0 if gains is None else float(gains[c])
        if g == 0.0:
            continue
        accumulate_speaker(out, np.asarray(x[c], dtype=np.float64) * g, positions[c], sr)
    return out


def accumulate_speaker(out: npt.NDArray, feed: npt.NDArray, position: tuple[float, float],
                       sr: int) -> None:
    """Add one speaker feed's binaural contribution into ``out`` ``(2, n)`` in
    place (overlap-add convolution, so a long feed never needs a full-length
    FFT buffer)."""
    ir_l, ir_r = speaker_ear_irs(position[0], position[1], sr)
    n = out.shape[1]
    out[0] += sig.oaconvolve(feed, ir_l)[:n]
    out[1] += sig.oaconvolve(feed, ir_r)[:n]


def build_hrir_asset(path: str = _ASSET_PATH) -> dict[str, Any]:
    """Bake the default ±30° HRIR pair at 44.1 kHz into a compact .npz asset."""
    os.makedirs(os.path.dirname(path), exist_ok=True)
    ir_ipsi, ir_contra = synth_hrir(_DEFAULT_AZIMUTH, _ASSET_SR)
    np.savez_compressed(path, ir_ipsi=ir_ipsi.astype(np.float32),
                        ir_contra=ir_contra.astype(np.float32),
                        sr=_ASSET_SR, azimuth_deg=_DEFAULT_AZIMUTH)
    return {"path": path, "sr": _ASSET_SR, "azimuth_deg": _DEFAULT_AZIMUTH, "n_taps": len(ir_ipsi)}
