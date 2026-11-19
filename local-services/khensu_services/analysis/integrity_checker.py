# local-services/khensu_services/analysis/integrity_checker.py
"""Audio file integrity checks.

Provides deterministic, fast checks that do not require heavy analysis:
  - SHA-256 hash for cache invalidation and reproducibility
  - Sample-rate consistency (detects material upsampled from a lower SR)
  - Bit-depth integrity (detects 16-bit content padded to 24/32-bit)

All functions accept a file path and return plain dicts suitable for JSON.
"""
from __future__ import annotations

from pathlib import Path
import hashlib
import math
from typing import Any

# Samples per chunk for the 16-bit residual check (bounds its temporaries).
_RESIDUAL_CHUNK = 1 << 18


def compute_sha256(file_path: str) -> str:
    """Return SHA-256 hex digest of a file without loading it into audio frames."""
    h = hashlib.sha256()
    with open(file_path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


# Upsample check: frame-averaged power spectrum (bounded memory; the old
# full-length rFFT of a long file was hundreds of MB). (DSP-18)
_UPSAMPLE_NFFT = 8192
_COMMON_RATES = (8000, 11025, 16000, 22050, 32000, 44100, 48000, 88200, 96000, 176400, 192000)
# Source rates the cliff test looks for. 32 kHz is left out: on a 44.1 kHz
# file a 16 kHz edge is far more often an MP3 encoder's low-pass than a
# 32 kHz original.
_CLIFF_SOURCE_RATES = (8000, 11025, 16000, 22050, 44100, 48000, 88200, 96000)
_CLIFF_MIN_DROP_DB = 30.0
_LOSSY_CONTAINERS = {"MP3", "OGG", "OPUS", "M4A", "AAC", "WMA", "MP2", "WEBM"}
_LOSSY_SUBTYPES = {"VORBIS", "OPUS", "MPEG_LAYER_I", "MPEG_LAYER_II", "MPEG_LAYER_III"}


def _residual_lsb_rms(ch, chunk: int = _RESIDUAL_CHUNK) -> float:
    """RMS (in 16-bit LSBs) of one channel's distance to the 16-bit grid,
    computed on the channel's own samples - not on an L/R average, whose
    half-LSB steps hid every padded stereo file (DSP-05)."""
    import numpy as np

    n = ch.shape[0]
    if n == 0:
        return 0.0
    acc = 0.0
    for s in range(0, n, chunk):
        q = np.asarray(ch[s:s + chunk], dtype=np.float64) * 32768.0   # exact (power of two)
        r = q - np.round(q)
        acc += float(np.dot(r, r))
    return math.sqrt(acc / n)


def check_file_integrity(file_path: str) -> dict[str, Any]:
    """Run all integrity checks on an audio file.

    Returns:
        sha256:               SHA-256 hex digest (use for cache invalidation)
        nominal_sample_rate:  SR as reported by the container
        nominal_bit_depth:    subtype string from soundfile (e.g. 'PCM_24')
        duration_sec:         total duration in seconds
        suspect_upsample:     True when spectral content stops well short of
                              Nyquist AND the implied original rate is a
                              common rate below the container's
        upsample_hf_ratio:    energy in the top 10% of the spectrum / energy in
                              the 60-90% band (genuine content: >0.20;
                              upsampled stub: <0.05)
        suspect_16bit_padded: True when EVERY channel of a 24/32-bit or float
                              file sits on the 16-bit grid (the source was
                              16-bit precision)
        lsb_residual_rms:     largest per-channel RMS distance to the 16-bit
                              grid, in full-scale units
        warnings:             list of human-readable warning strings
    """
    import numpy as np
    import soundfile as sf

    from khensu_services.analysis.dsp_metrics import averaged_power_spectrum, pearson

    sha256 = compute_sha256(file_path)
    try:
        info = sf.info(file_path)
        subtype, container, nominal_channels = info.subtype, info.format, int(info.channels)
        # 32-bit integer / double samples need float64 to keep their low bits;
        # 16/24-bit PCM and float32 are exact in float32.
        dtype = "float64" if subtype in ("PCM_32", "DOUBLE") else "float32"
        y, sr = sf.read(file_path, dtype=dtype, always_2d=True)
        chans = y.T                             # (channels, samples) view
    except sf.LibsndfileError:
        # libsndfile cannot open every MP3 (one with a large ID3 tag failed
        # under the 2026-11-18 stress test, a 500): decode it like every
        # other scan instead. A lossy file has no meaningful bit depth, so
        # the 16-bit-padding check below does not apply.
        from khensu_services.analysis.context import load_audio

        decoded, sr = load_audio(file_path, sr=None, mono=False)
        chans = np.atleast_2d(decoded)          # (channels, samples)
        subtype, container = "unknown", Path(file_path).suffix.lstrip(".").upper()
        nominal_channels = int(chans.shape[0])

    # Collapse to mono for spectral analysis
    mono = chans.mean(axis=0) if chans.shape[0] > 1 else chans[0]
    n    = len(mono)
    duration_sec = float(n / sr)

    warnings: list[str] = []

    # -------------------------------------------------------------------------
    # Sample-rate consistency check
    # Upsampled audio has near-zero energy above the original-capture Nyquist.
    # We measure the top 10% of the spectrum against the 60-90% band.
    # -------------------------------------------------------------------------
    power    = averaged_power_spectrum(mono, _UPSAMPLE_NFFT)
    nyq_idx  = len(power) - 1
    p60_idx  = max(1, int(nyq_idx * 0.60))
    p90_idx  = max(1, int(nyq_idx * 0.90))

    energy_top   = float(np.sum(power[p90_idx:]) + 1e-12)
    energy_mid   = float(np.sum(power[p60_idx:p90_idx]) + 1e-12)

    upsample_hf_ratio = energy_top / (energy_mid + 1e-12)
    energy_total = float(np.sum(power)) + 1e-30
    suspect_upsample = False
    # 2026-11-18: the old test ("the highest bin within 10 dB of the mid peak")
    # fired on 95 % of MP3 / Ogg / Opus files and 68 % of FLAC masters: music
    # rolls off long before Nyquist. A resampler leaves a CLIFF at the source
    # rate's Nyquist with a flat floor above it; that is what is tested now.
    # A lossy codec's low-pass is a cliff by design, so a lossy container is
    # not tested at all (upsampling cannot be told from the encoder).
    lossy = container.upper() in _LOSSY_CONTAINERS or subtype.upper() in _LOSSY_SUBTYPES
    # Any signal at all: the old 40-60 % band gate hid exactly the low-rate
    # sources (16 kHz content stops below it).
    if energy_total > 1e-20 and not lossy:
        suspected_original_sr = _upsample_cliff(power, int(sr))
        suspect_upsample = suspected_original_sr is not None
        if suspect_upsample:
            warnings.append(
                f"Audio may have been upsampled from ~{suspected_original_sr:,} Hz "
                f"(HF energy ratio = {upsample_hf_ratio:.4f}). "
                "True bit depth and sample rate may differ from container metadata."
            )

    # -------------------------------------------------------------------------
    # Bit-depth integrity check
    # For 24/32-bit container subtypes, check whether every channel's samples
    # sit on the 16-bit grid - i.e. the source was 16-bit material. (DSP-05)
    # -------------------------------------------------------------------------
    higher_depth_subtypes = {"PCM_24", "PCM_32", "FLOAT", "DOUBLE"}
    suspect_16bit_padded  = False
    lsb_residual_rms      = 0.0

    if subtype in higher_depth_subtypes:
        lsb_16 = 1.0 / 32768.0
        per_channel = [_residual_lsb_rms(chans[c]) for c in range(chans.shape[0])]
        lsb_residual_rms = float(max(per_channel) * lsb_16) if per_channel else 0.0
        has_signal = bool(np.any(chans != 0.0))
        # Padded when < 1% of an LSB off the grid on EVERY channel.
        suspect_16bit_padded = bool(per_channel and has_signal
                                    and all(r < 0.01 for r in per_channel))
        if suspect_16bit_padded:
            warnings.append(
                f"File declared as {subtype} but every channel's samples sit on "
                f"the 16-bit grid (largest residual RMS = {lsb_residual_rms:.2e}, "
                "below 1% of a 16-bit LSB). "
                "Source material was likely recorded or bounced at 16-bit depth."
            )

    # -------------------------------------------------------------------------
    # Channel count / format sanity check
    # -------------------------------------------------------------------------
    # Mono file declared as stereo: both channels carry the same signal.
    suspect_fake_stereo = False
    if nominal_channels >= 2 and chans.shape[0] >= 2:
        left, right = chans[0], chans[1]
        corr_check = pearson(left, right, silent=0.0)
        if corr_check > 0.9999:
            suspect_fake_stereo = True
            peak = max(float(np.max(np.abs(left))), float(np.max(np.abs(right))), 1e-12)
            if float(np.max(np.abs(left - right))) <= 1e-6 * peak:
                warnings.append(
                    "File has 2 channels but left and right are identical "
                    f"(correlation = {corr_check:.6f}). This is likely a mono file "
                    "bounced to stereo — wasted file size and may confuse stereo analysers."
                )
            else:
                # Same signal at different levels: corrcoef ignores gain, so
                # this is a mono source with a level offset, not identical
                # channels. (DSP-18)
                rms_l = float(np.sqrt(np.mean(np.square(left, dtype=np.float64))))
                rms_r = float(np.sqrt(np.mean(np.square(right, dtype=np.float64))))
                offset_db = 20.0 * math.log10((rms_l + 1e-12) / (rms_r + 1e-12))
                warnings.append(
                    "File has 2 channels carrying the same signal at different levels "
                    f"(correlation = {corr_check:.6f}, L-R offset = {offset_db:+.2f} dB). "
                    "This is likely a mono source panned or gain-offset into stereo."
                )
    # More than 2 channels in a standard stereo deliverable format
    if nominal_channels > 2 and container in ("WAV", "AIFF", "FLAC"):
        warnings.append(
            f"File contains {nominal_channels} channels in a {container} container. "
            "Standard stereo delivery files should have exactly 2 channels. "
            "Verify the export settings if this is intended as a stereo master."
        )

    return {
        "sha256":               sha256,
        "nominal_sample_rate":  int(sr),
        "nominal_bit_depth":    subtype,
        "nominal_channels":     nominal_channels,
        "duration_sec":         round(duration_sec, 3),
        "suspect_upsample":     suspect_upsample,
        "upsample_hf_ratio":    round(upsample_hf_ratio, 4),
        "suspect_16bit_padded": suspect_16bit_padded,
        "lsb_residual_rms":     lsb_residual_rms,
        "suspect_fake_stereo":  suspect_fake_stereo,
        "warnings":             warnings,
    }


def _upsample_cliff(power: "Any", sr: int) -> int | None:
    """The source rate a resampler left a cliff for, or None.

    For each common rate below ``sr``, its Nyquist ``edge`` is a cliff when
    the band just below it (0.75-0.9 x edge) is at least
    ``_CLIFF_MIN_DROP_DB`` louder than EVERYTHING from past the filter's
    transition band (1.15 x edge) to the container's Nyquist (the loudest
    1 %, so one stray tone does not count). Music's natural roll-off fails
    that: cymbals and air keep it within 30 dB well above the edge. Rates
    are tried from the highest down, so the closest source rate wins.
    """
    import numpy as np

    n = len(power)
    nyq = sr / 2.0
    freqs = np.linspace(0.0, nyq, n)
    db = 10.0 * np.log10(np.asarray(power, dtype=np.float64) + 1e-30)
    for rate in sorted((r for r in _CLIFF_SOURCE_RATES if r < sr), reverse=True):
        edge = rate / 2.0
        below = (freqs >= 0.75 * edge) & (freqs < 0.9 * edge)
        above = (freqs >= 1.15 * edge) & (freqs <= nyq)
        if below.sum() < 8 or above.sum() < 8:
            continue
        level_below = float(np.median(db[below]))
        level_above = float(np.percentile(db[above], 99.0))
        if level_below - level_above >= _CLIFF_MIN_DROP_DB:
            return rate
    return None
