"""Streaming playback preview (CE-DEL-3): what a listener actually hears.

    master -> codec round trip -> platform normalisation (+ limiter) -> device

1. **Codec.** With ffmpeg on PATH the master is really encoded (AAC, Opus or
   MP3 at the requested bitrate) and decoded back, each call bounded by a
   timeout. Without ffmpeg - or when the encode fails - the codec-artifact
   approximation (``codec_artifacts.simulate_codec``) stands in and the
   result says ``codec_mode: "simulated"`` with the reason. The decoded
   signal is time-aligned to the master (encoder priming delay removed by
   cross-correlation) so A/B switching does not jump.
2. **Codec damage**, measured on the aligned decode: the codec's added
   distortion (residual after the best-fit gain, as dB below the signal),
   the true peak after decode against the master's, and the existing
   artifact metrics (HF / air loss, width change, pre-echo).
3. **Platform normalisation** from ``streaming_normalization``: the gain the
   platform applies to this master (measured on the master, as the platform
   does), with the platform's limiter where it has one (Spotify Loud).
4. **Device** profile from ``copilot/device_sim`` (phone, earbuds, car, club),
   optional.

Three float WAVs are written for the A/B:

* ``preview_path`` - the full chain at the level the listener gets;
* ``reference_path`` - the master through the same normalisation and device
  but NO codec (A);
* ``matched_preview_path`` - the preview gain-matched to the reference's
  integrated loudness (B), so the A/B isolates the codec, not level.
"""
from __future__ import annotations

import math
import os
import shutil
import subprocess
import tempfile
from typing import Any, Optional

import numpy as np

_ALIGN_SEARCH_SEC = 0.2      # encoder delay search window (priming is < 50 ms)
_ALIGN_WINDOW_SEC = 12.0
_FFMPEG_BASE_TIMEOUT = 60.0


class UnsupportedLayoutError(ValueError):
    """A readable source in a channel layout the preview does not render
    (multichannel): a deliberate refusal (409), not an invalid request."""


def ffmpeg_path() -> Optional[str]:
    return shutil.which("ffmpeg")


def _r(v: Optional[float], nd: int = 2) -> Optional[float]:
    return round(float(v), nd) if v is not None and math.isfinite(float(v)) else None


def _measure(y: np.ndarray, sr: int) -> dict[str, Optional[float]]:
    """Integrated LUFS and true peak of a ``(frames, channels)`` buffer."""
    from khensu_services.analysis.loudness import integrated_lufs, true_peak_dbtp
    x = np.asarray(y, dtype=np.float64).T
    return {"integrated_lufs": _r(integrated_lufs(x, sr)),
            "true_peak_dbtp": _r(true_peak_dbtp(x)) if x.size else None}


def align_to(reference: np.ndarray, signal: np.ndarray, sr: int) -> tuple[np.ndarray, int]:
    """``signal`` shifted to line up with ``reference`` (both ``(frames, ch)``)
    and cut / zero-padded to its length. Returns ``(aligned, lag)`` where a
    positive lag means ``signal`` was late by ``lag`` samples."""
    from scipy.signal import correlate

    n = reference.shape[0]
    w = min(n, signal.shape[0], int(_ALIGN_WINDOW_SEC * sr))
    max_lag = int(_ALIGN_SEARCH_SEC * sr)
    lag = 0
    if w > 32:
        a = reference[:w].mean(axis=1)
        b = signal[:min(signal.shape[0], w + max_lag)].mean(axis=1)
        if np.any(a) and np.any(b):
            xc = correlate(b, a, mode="full", method="fft")
            zero = a.shape[0] - 1
            lo, hi = max(0, zero - max_lag), min(xc.shape[0], zero + max_lag + 1)
            lag = int(np.argmax(xc[lo:hi]) + lo - zero)
    if lag >= 0:
        out = signal[lag:lag + n]
    else:
        out = np.concatenate([np.zeros((-lag, signal.shape[1])), signal[:n + lag]], axis=0)
    if out.shape[0] < n:
        out = np.concatenate([out, np.zeros((n - out.shape[0], signal.shape[1]))], axis=0)
    return np.asarray(out, dtype=np.float64), lag


def codec_distortion_db(reference: np.ndarray, decoded: np.ndarray) -> Optional[float]:
    """Residual energy after the least-squares gain, in dB relative to the
    reference (more negative = cleaner): what the codec added or removed
    beyond a level change."""
    s = np.asarray(reference, dtype=np.float64).ravel()
    d = np.asarray(decoded, dtype=np.float64).ravel()
    es = float(np.dot(s, s))
    if es <= 0:
        return None
    g = float(np.dot(s, d)) / es
    r = d - g * s
    er = float(np.dot(r, r))
    return 10.0 * math.log10(er / es) if er > 0 else -150.0


def stream_preview(file_path: str, platform: str, codec: str = "", bitrate_kbps: int = 0,
                   device: str = "", output_dir: Optional[str] = None,
                   use_ffmpeg: bool = True) -> dict[str, Any]:
    """Blocking. See the module doc. Raises ``ValueError`` for bad input."""
    import soundfile as sf

    from khensu_services.analysis.codec_artifacts import (
        MAX_BITRATE_K, MIN_BITRATE_K, PLATFORM_STREAM_DEFAULTS, STREAM_CODECS,
        ffmpeg_round_trip, measure_codec_artifacts, simulate_codec)
    from khensu_services.analysis.delivery_targets import canonical_platform_id
    from khensu_services.analysis.streaming_normalization import normalization_gain_db, playback_policy
    from khensu_services.copilot.device_sim import apply_device_profile, is_device
    from khensu_services.copilot.dsp import _atomic_render_write, require_finite_audio

    pid = canonical_platform_id(platform)
    spec = playback_policy(pid)
    if spec is None:
        raise ValueError(f"Unknown streaming platform: {platform}")
    d_codec, d_rate = PLATFORM_STREAM_DEFAULTS.get(spec["id"], ("aac", 256))
    codec = (codec or d_codec).lower()
    if codec not in STREAM_CODECS:
        raise ValueError(f"codec must be one of: {', '.join(STREAM_CODECS)}")
    bitrate = int(bitrate_kbps or d_rate)
    if not MIN_BITRATE_K <= bitrate <= MAX_BITRATE_K:
        raise ValueError(f"bitrate_kbps must be {MIN_BITRATE_K}-{MAX_BITRATE_K}")
    if device and not is_device(device):
        raise ValueError(f"Unknown device profile: {device}")

    src, sr = sf.read(file_path, dtype="float32", always_2d=True)
    sr = int(sr)
    if src.shape[1] > 2:
        raise UnsupportedLayoutError("Stream preview takes a mono or stereo master; use the immersive "
                                     "check for multichannel files.")
    require_finite_audio(src, "stream preview source")
    master = src.astype(np.float64)
    notes: list[str] = []

    out_dir = output_dir or os.path.join(os.path.dirname(os.path.abspath(file_path)), "stream_preview")
    os.makedirs(out_dir, exist_ok=True)
    stem = os.path.splitext(os.path.basename(file_path))[0]
    tag = f"{spec['id']}_{codec}{bitrate}" + (f"_{device}" if device else "")

    # 1. codec round trip
    mode, reason, encoded_path = "simulated", "", None
    decoded: Optional[np.ndarray] = None
    if use_ffmpeg and ffmpeg_path():
        timeout = _FFMPEG_BASE_TIMEOUT + 2.0 * src.shape[0] / sr
        work = tempfile.mkdtemp(prefix="khensu_stream_")
        try:
            pcm = os.path.join(work, "master.wav")
            sf.write(pcm, src, sr, subtype="FLOAT")
            enc = os.path.join(out_dir, f"{stem}_stream_{tag}.{STREAM_CODECS[codec]['ext']}")
            dec = os.path.join(work, "decoded.wav")
            ffmpeg_round_trip(pcm, codec, bitrate, sr, enc, dec, timeout)
            y, dsr = sf.read(dec, dtype="float32", always_2d=True)
            if int(dsr) != sr:
                raise RuntimeError(f"decoder returned {dsr} Hz")
            if y.shape[1] != src.shape[1]:
                y = np.repeat(y.mean(axis=1, keepdims=True), src.shape[1], axis=1)
            decoded, mode, encoded_path = y.astype(np.float64), "ffmpeg", enc
        except subprocess.TimeoutExpired:
            reason = "ffmpeg timed out; the codec was simulated instead."
        except (subprocess.CalledProcessError, OSError, RuntimeError) as exc:
            err = getattr(exc, "stderr", b"") or b""
            reason = ("ffmpeg could not encode " + STREAM_CODECS[codec]["name"] + ": "
                      + (err.decode("utf-8", "replace").strip()[:200] or str(exc)[:200])
                      + " - the codec was simulated instead.")
        finally:
            shutil.rmtree(work, ignore_errors=True)
    elif use_ffmpeg:
        reason = "ffmpeg is not on PATH, so the codec was simulated (install ffmpeg for a real encode)."
    else:
        reason = "A real encode was not requested; the codec was simulated."
    if decoded is None:
        decoded = simulate_codec(master, sr, codec, bitrate)
    aligned, lag = align_to(master, decoded, sr)

    m_master = _measure(master, sr)
    m_decoded = _measure(aligned, sr)
    artifacts = measure_codec_artifacts(master, aligned, sr)
    dist = codec_distortion_db(master, aligned)
    tp_delta = (m_decoded["true_peak_dbtp"] - m_master["true_peak_dbtp"]
                if m_decoded["true_peak_dbtp"] is not None and m_master["true_peak_dbtp"] is not None else None)
    if m_decoded["true_peak_dbtp"] is not None and m_decoded["true_peak_dbtp"] > 0.0:
        notes.append(f"The decoded stream peaks at {m_decoded['true_peak_dbtp']:+.2f} dBTP: the "
                     "listener's player clips unless the platform turns it down.")

    # 2. platform normalisation (gain measured on the master, as platforms do)
    gain = normalization_gain_db(m_master["integrated_lufs"], spec, m_master["true_peak_dbtp"])
    g_lin = 10.0 ** (gain / 20.0)
    limiter = False

    def _platform(y: np.ndarray) -> np.ndarray:
        nonlocal limiter
        out = y * g_lin
        if spec.get("limited") and gain > 0.0:
            from khensu_services.copilot.tp_limiter import true_peak_limit
            from khensu_services.analysis.loudness import true_peak_dbtp
            if true_peak_dbtp(out.T) > float(spec["ceiling_dbtp"]):
                limiter = True
                out = true_peak_limit(out.T, sr, float(spec["ceiling_dbtp"])).T
        return out

    streamed = _platform(aligned)
    reference = _platform(master)
    m_platform = _measure(streamed, sr)

    # 3. device
    if device:
        streamed = apply_device_profile(streamed, sr, device)
        reference = apply_device_profile(reference, sr, device)
    m_final = _measure(streamed, sr)
    m_ref = _measure(reference, sr)
    match_db = 0.0
    if m_final["integrated_lufs"] is not None and m_ref["integrated_lufs"] is not None:
        match_db = m_ref["integrated_lufs"] - m_final["integrated_lufs"]
    matched = streamed * 10.0 ** (match_db / 20.0)

    def _write(name: str, data: np.ndarray) -> str:
        dst = os.path.join(out_dir, name)
        _atomic_render_write(dst, lambda tmp: sf.write(tmp, data.astype(np.float32), sr, subtype="FLOAT"))
        return dst

    preview_path = _write(f"{stem}_stream_{tag}_preview.wav", streamed)
    matched_path = _write(f"{stem}_stream_{tag}_matched.wav", matched)
    reference_path = _write(f"{stem}_stream_{tag}_reference.wav", reference)
    if mode == "simulated":
        notes.append("Codec SIMULATED: band-limit + signal-shaped quantisation noise, not a real encoder.")
    if limiter:
        notes.append(f"{spec['name']} turns this master up through its limiter.")

    out: dict[str, Any] = {
        "file_path": file_path,
        "platform": spec["id"],
        "platform_name": spec["name"],
        "codec": codec,
        "bitrate_kbps": bitrate,
        "device": device or None,
        "codec_mode": mode,
        "sample_rate": sr,
        "alignment_lag_samples": int(lag),
        "master": m_master,
        "decoded": m_decoded,
        "after_platform": m_platform,
        "final": m_final,
        "reference": m_ref,
        "codec_distortion_db": _r(dist),
        "true_peak_delta_db": _r(tp_delta),
        "artifacts": artifacts,
        "platform_gain_db": _r(gain),
        "limiter_engaged": bool(limiter),
        "match_gain_db": _r(match_db),
        "preview_path": preview_path,
        "matched_preview_path": matched_path,
        "reference_path": reference_path,
        "notes": notes,
    }
    if reason:
        out["simulated_reason" if mode == "simulated" else "note"] = reason
    if encoded_path:
        out["encoded_path"] = encoded_path
    return out
