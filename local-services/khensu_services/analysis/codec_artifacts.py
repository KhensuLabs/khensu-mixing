"""Codec / transcode artifact audit.

Encodes the master through a streaming platform's lossy codec at its delivery
bitrate, decodes it back to PCM, and measures the artifacts the codec added so
the user can A/B "the Spotify encode" against their master. The metrics are
deliberately shift-tolerant global statistics (band-energy ratios, stereo
width, true peak) so the encoder's sample delay doesn't corrupt them:

* hf_loss_db   — energy lost above 16 kHz (the first thing a codec throws away)
* air_loss_db  — energy lost in the 10-16 kHz "air" band (audible dulling)
* stereo_width_delta — change in shared stereo width (joint-stereo can collapse width)
* true_peak_delta_dbtp — codecs can raise inter-sample peaks past the ceiling
* pre_echo_score — transient pre-smearing proxy (0 = none)

Encoding needs ffmpeg on PATH; when it is missing the audit reports
``encoder_available: False`` rather than failing. The measurement itself
(``measure_codec_artifacts``) is pure numpy and always available.
"""
from __future__ import annotations

import os
import shutil
import subprocess
from typing import Any, Optional

import numpy as np
import soundfile as sf
from khensu_services.common.job_status import JobStatus

# Platform delivery codecs + bitrates. AAC/Opus need ffmpeg; kept small + honest.
PLATFORM_CODECS: dict[str, dict[str, Any]] = {
    "spotify": {"name": "Spotify (AAC 256)",  "codec": "aac",         "bitrate_k": 256, "ext": "m4a"},
    "apple":   {"name": "Apple (AAC 256)",    "codec": "aac",         "bitrate_k": 256, "ext": "m4a"},
    "youtube": {"name": "YouTube (Opus 128)", "codec": "libopus",     "bitrate_k": 128, "ext": "opus"},
    "mp3_320": {"name": "MP3 320",            "codec": "libmp3lame",  "bitrate_k": 320, "ext": "mp3"},
    "mp3_128": {"name": "MP3 128",            "codec": "libmp3lame",  "bitrate_k": 128, "ext": "mp3"},
}
DEFAULT_CODECS = ["spotify", "youtube", "mp3_128"]


def encoder_available() -> bool:
    return shutil.which("ffmpeg") is not None


def _band_energy(y_frames: np.ndarray, sr: int, lo_hz: float, hi_hz: float) -> float:
    """Mean spectral energy in [lo, hi) Hz over a mono downmix."""
    mono = y_frames.mean(axis=1) if y_frames.ndim > 1 else y_frames
    n = int(min(len(mono), 1 << 20))
    if n < 16:
        return 0.0
    spec = np.abs(np.fft.rfft(mono[:n]))
    freqs = np.fft.rfftfreq(n, 1.0 / sr)
    mask = (freqs >= lo_hz) & (freqs < hi_hz)
    if not np.any(mask):
        return 0.0
    return float(np.mean(spec[mask] ** 2))


def _width(y_frames: np.ndarray) -> float:
    """Shared stereo width (dsp_metrics: 0 mono .. 1 uncorrelated); 0 for mono."""
    from khensu_services.analysis.dsp_metrics import stereo_width_frames
    return stereo_width_frames(y_frames)


def _true_peak_dbtp(y_frames: np.ndarray, sr: int) -> float:
    from khensu_services.analysis.signal_integrity import analyze_true_peak
    return float(analyze_true_peak(y_frames.T, sr)["true_peak_dbtp"])


def _pre_echo_score(dry: np.ndarray, decoded: np.ndarray, sr: int) -> float:
    """Proxy for transient pre-smearing: energy the codec added in the 5 ms
    window just before the mix's largest transient. 0 = no pre-echo."""
    m_dry = dry.mean(axis=1) if dry.ndim > 1 else dry
    m_dec = decoded.mean(axis=1) if decoded.ndim > 1 else decoded
    n = int(min(len(m_dry), len(m_dec)))
    if n < sr // 10:
        return 0.0
    m_dry, m_dec = m_dry[:n], m_dec[:n]
    peak_i = int(np.argmax(np.abs(m_dry)))
    w = int(sr * 0.005)
    lo = max(0, peak_i - w)
    if peak_i - lo < 8:
        return 0.0
    e_dry = float(np.mean(m_dry[lo:peak_i] ** 2)) + 1e-12
    e_dec = float(np.mean(m_dec[lo:peak_i] ** 2))
    return round(max(0.0, 10.0 * np.log10(e_dec / e_dry)), 2)


def measure_codec_artifacts(dry: np.ndarray, decoded: np.ndarray, sr: int) -> dict[str, Any]:
    """Compare a dry master to its decoded-after-encode version. Pure numpy."""
    nyq = sr / 2.0
    hf_hi = min(20000.0, nyq)
    dry_hf = _band_energy(dry, sr, 16000.0, hf_hi)
    dec_hf = _band_energy(decoded, sr, 16000.0, hf_hi)
    dry_air = _band_energy(dry, sr, 10000.0, 16000.0)
    dec_air = _band_energy(decoded, sr, 10000.0, 16000.0)

    def loss_db(a: float, b: float) -> float:
        if a <= 0.0:
            return 0.0
        return round(10.0 * float(np.log10(a / (b + 1e-12))), 2)

    return {
        "hf_loss_db": loss_db(dry_hf, dec_hf),
        "air_loss_db": loss_db(dry_air, dec_air),
        "stereo_width_delta": round(_width(decoded) - _width(dry), 3),
        "true_peak_delta_dbtp": round(_true_peak_dbtp(decoded, sr) - _true_peak_dbtp(dry, sr), 2),
        "pre_echo_score": _pre_echo_score(dry, decoded, sr),
    }


def _run_ffmpeg(args: list[str], timeout: float | None = None) -> None:
    subprocess.run(["ffmpeg", "-nostdin", "-y", "-hide_banner", "-loglevel", "error", *args],
                   check=True, capture_output=True, timeout=timeout)


# --- Stream preview codecs (CE-DEL-3) ------------------------------------------
#
# The codec families /delivery/stream_preview can round-trip, by the name the
# request uses: the ffmpeg encoder and the container extension.
STREAM_CODECS: dict[str, dict[str, str]] = {
    "aac": {"encoder": "aac", "ext": "m4a", "name": "AAC"},
    "opus": {"encoder": "libopus", "ext": "opus", "name": "Opus"},
    "mp3": {"encoder": "libmp3lame", "ext": "mp3", "name": "MP3"},
}
# What each streaming platform serves by default (its standard-quality tier),
# by delivery-target id; a request may override codec and bitrate.
PLATFORM_STREAM_DEFAULTS: dict[str, tuple[str, int]] = {
    "spotify": ("aac", 256), "spotify_loud": ("aac", 256), "apple_music": ("aac", 256),
    "youtube": ("opus", 128), "tidal": ("aac", 320), "amazon_music": ("aac", 256),
    "deezer": ("mp3", 320),
}
MIN_BITRATE_K, MAX_BITRATE_K = 32, 512


def ffmpeg_round_trip(src_wav: str, codec: str, bitrate_k: int, sr: int, encoded_path: str,
                      decoded_path: str, timeout: float) -> None:
    """Encode ``src_wav`` with the real encoder and decode it back to a
    float WAV at ``sr``. Each ffmpeg call is bounded by ``timeout`` seconds
    (``subprocess.TimeoutExpired`` propagates)."""
    spec = STREAM_CODECS[codec]
    _run_ffmpeg(["-i", src_wav, "-vn", "-c:a", spec["encoder"], "-b:a", f"{int(bitrate_k)}k",
                 encoded_path], timeout=timeout)
    _run_ffmpeg(["-i", encoded_path, "-vn", "-ar", str(int(sr)), "-c:a", "pcm_f32le", decoded_path],
                timeout=timeout)


def _sim_cutoff_hz(codec: str, bitrate_k: int, channels: int) -> float:
    """Encoder low-pass a codec typically applies at this bitrate (per the
    LAME / FFmpeg-AAC / libopus defaults, rounded)."""
    per_ch = bitrate_k / max(1, channels)
    if codec == "opus":
        return 20000.0 if bitrate_k >= 64 else 12000.0
    if codec == "mp3":
        for kbps, hz in ((160, 20000.0), (128, 19500.0), (96, 17500.0), (64, 16000.0)):
            if per_ch >= kbps / 2:
                return hz
        return 11000.0
    for kbps, hz in ((128, 20000.0), (96, 18000.0), (64, 16000.0)):    # aac
        if per_ch >= kbps:
            return hz
    return 14000.0


def _sim_snr_db(codec: str, bitrate_k: int) -> float:
    """Signal-to-quantisation-noise per band the simulation adds."""
    offset = {"opus": 3.0, "aac": 1.5, "mp3": -2.0}.get(codec, 0.0)
    return float(np.clip(8.0 + 0.09 * bitrate_k + offset, 10.0, 42.0))


def simulate_codec(y: np.ndarray, sr: int, codec: str, bitrate_k: int, seed: int = 0x4B4D) -> np.ndarray:
    """Approximate a lossy codec round trip WITHOUT an encoder (no ffmpeg):
    the encoder's band-limit, signal-shaped quantisation noise per STFT band
    at a bitrate-dependent SNR, and joint-stereo side loss in the top octave
    at low bitrates. ``y`` and the result are ``(frames, channels)``. It is
    an approximation for the A/B, labelled ``simulated`` by every caller -
    never a measurement of a real encoder."""
    from scipy.signal import butter, istft, sosfiltfilt, stft

    x = np.asarray(y, dtype=np.float64)
    n, ch = x.shape
    cutoff = min(_sim_cutoff_hz(codec, bitrate_k, ch), 0.45 * sr)
    sos = butter(8, cutoff, btype="low", fs=sr, output="sos")
    out = sosfiltfilt(sos, x, axis=0) if n > 64 else x.copy()

    nfft, hop = 2048, 1024
    if n >= nfft:
        rng = np.random.default_rng(seed)
        snr_lin = 10.0 ** (-_sim_snr_db(codec, bitrate_k) / 20.0)
        freqs = np.fft.rfftfreq(nfft, 1.0 / sr)
        # Third-octave-ish bands from 50 Hz: noise follows each band's level.
        edges = [0.0] + [50.0 * 2 ** (k / 3.0) for k in range(0, 40)
                         if 50.0 * 2 ** (k / 3.0) < sr / 2] + [sr / 2 + 1]
        band_of = np.searchsorted(np.asarray(edges), freqs, side="right") - 1
        for c in range(ch):
            _, _, Z = stft(out[:, c], fs=sr, nperseg=nfft, noverlap=nfft - hop, window="hann")
            mag = np.abs(Z)
            noise = np.zeros_like(mag)
            for b in np.unique(band_of):
                rows = band_of == b
                level = np.sqrt(np.mean(mag[rows] ** 2, axis=0, keepdims=True))
                noise[rows] = level * snr_lin
            phase = np.exp(2j * np.pi * rng.random(Z.shape))
            Z = Z + noise * phase * (freqs[:, None] <= cutoff)
            _, rec = istft(Z, fs=sr, nperseg=nfft, noverlap=nfft - hop, window="hann")
            out[:, c] = rec[:n] if rec.shape[0] >= n else np.pad(rec, (0, n - rec.shape[0]))
    if ch == 2 and bitrate_k / 2 < 96:
        # Joint stereo spends few bits on the side signal up top.
        mid, side = (out[:, 0] + out[:, 1]) / 2, (out[:, 0] - out[:, 1]) / 2
        hp = butter(4, min(10000.0, 0.4 * sr), btype="high", fs=sr, output="sos")
        side = side - 0.5 * sosfiltfilt(hp, side) if n > 64 else side
        out = np.column_stack([mid + side, mid - side])
    return out


def encode_and_decode(src_path: str, codec: str, bitrate_k: int, ext: str,
                      sr: int, out_dir: str) -> str:
    """Encode src to the codec then decode back to a comparison WAV. Returns the
    decoded WAV path. Requires ffmpeg. The encoded file is kept next to it for
    auditioning ("the Spotify encode")."""
    base = os.path.splitext(os.path.basename(src_path))[0]
    encoded = os.path.join(out_dir, f"{base}_{codec}_{bitrate_k}k.{ext}")
    decoded = os.path.join(out_dir, f"{base}_{codec}_{bitrate_k}k_decoded.wav")
    _run_ffmpeg(["-i", src_path, "-c:a", codec, "-b:a", f"{bitrate_k}k", encoded])
    _run_ffmpeg(["-i", encoded, "-ar", str(sr), decoded])
    return decoded


def audit_codecs(src_path: str, codecs: Optional[list[str]] = None,
                 out_dir: Optional[str] = None) -> dict[str, Any]:
    """Encode the master through each requested platform codec and report the
    artifacts added. Never raises; reports encoder_available=False when ffmpeg
    is missing."""
    ids = [c for c in (codecs or DEFAULT_CODECS) if c in PLATFORM_CODECS]
    if not encoder_available():
        return {"encoder_available": False, "codecs": [c for c in ids],
                "results": [], "reason": "ffmpeg not found on PATH"}

    dry, sr = sf.read(src_path, always_2d=True)
    out_dir = out_dir or os.path.join(os.path.dirname(src_path), "codec_audit")
    os.makedirs(out_dir, exist_ok=True)

    results: list[dict[str, Any]] = []
    for cid in ids:
        spec = PLATFORM_CODECS[cid]
        try:
            decoded_path = encode_and_decode(src_path, spec["codec"], spec["bitrate_k"],
                                             spec["ext"], sr, out_dir)
            decoded, _ = sf.read(decoded_path, always_2d=True)
            metrics = measure_codec_artifacts(dry, decoded, sr)
            results.append({"codec_id": cid, "name": spec["name"], "status": JobStatus.COMPLETED.value,
                            "decoded_path": decoded_path, **metrics})
        except Exception as exc:  # a single codec failing must not sink the audit
            results.append({"codec_id": cid, "name": spec["name"], "status": JobStatus.FAILED.value,
                            "error": str(exc)})
    return {"encoder_available": True, "results": results}
