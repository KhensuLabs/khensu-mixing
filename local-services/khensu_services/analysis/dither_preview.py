"""Dither & noise-shaping preview.

Reducing bit depth (e.g. a 24-bit master delivered at 16-bit) requantises the
signal. Plain truncation produces quantisation *distortion* that correlates
with the signal (audible as gritty harmonics in quiet tails); dither replaces
it with a benign, decorrelated noise floor, and noise shaping pushes that noise
out of the ear's most sensitive band. This compares the three so the user can
see the noise-floor delta and audition the dithered result.

Metrics per mode (truncate / tpdf dither / shaped dither):
* noise_floor_dbfs      — RMS of the quantisation error (quantised - dry), dBFS
* distortion_correlation — |corr(error, signal)|; high = correlated distortion
                           (bad), near-zero = benign decorrelated noise (good)
"""
from __future__ import annotations

import os
from typing import Any, Optional

import numpy as np
import soundfile as sf

_METRIC_MAX_SEC = 20.0   # cap the metric window (bounds memory and time)


def _quantize_vectorised(y: np.ndarray, bits: int, dither: bool,
                         rng: np.random.Generator, truncate: bool = False) -> np.ndarray:
    q = float(2 ** (bits - 1))
    lsb = 1.0 / q
    x = y
    if dither:
        # Triangular-PDF dither, ±1 LSB (sum of two rectangular sources).
        x = y + (rng.random(y.shape) - rng.random(y.shape)) * lsb
    if truncate:
        # True bit truncation: dropping the low bits of a two's-complement
        # sample rounds toward NEGATIVE infinity (floor), not toward zero —
        # -1.5 LSB truncates to -2 LSB. np.trunc modelled sign-magnitude
        # truncation, which produces a different (symmetric, zero-centred)
        # error than real fixed-point hardware.
        return np.floor(x * q) / q
    return np.round(x * q) / q


def _quantize_shaped(y: np.ndarray, bits: int, rng: np.random.Generator) -> np.ndarray:
    """First-order error-feedback noise shaping + TPDF dither, vectorised.

    ``v = x - e[n-1]``, ``q = Q(v + d)``, ``e = q - v``: the fed-back error
    INCLUDES the dither (AUDIT-082), so the output error ``e[n] - e[n-1]`` is
    the whole requantisation noise - dither and rounding together - shaped by
    ``1 - z^-1``. Feeding back only the rounding error left the dither itself
    white, so the "shaped" mode was barely shaped at all.

    In LSB units the recursion has a closed form: with ``S[n] = sum x[0..n]``
    and ``Q[n] = sum q[0..n]`` (an integer), ``q[n] = round(S[n] - Q[n-1] +
    d[n])`` and so ``Q[n] = round(S[n] + d[n])`` - the integer can be pulled
    out of the rounding. Hence ``q = diff(round(cumsum(x) + d))``, with no
    per-sample Python loop (it took 7.7 s for the 20 s stereo window on a DSP
    worker; DSP-10). The dither is drawn in the same order as the old loop
    (per sample: one row of uniforms, then a second), so the same seed gives
    the same dither.
    """
    q = float(2 ** (bits - 1))
    y2 = np.atleast_2d(y.T).T if y.ndim > 1 else y[:, None]
    n, ch = y2.shape
    u = rng.random((n, 2, ch))
    d = u[:, 0, :] - u[:, 1, :]                       # TPDF, +/-1 LSB (in LSBs)
    cum = np.cumsum(np.asarray(y2, dtype=np.float64) * q, axis=0)
    Q = np.round(cum + d)
    out = np.diff(Q, axis=0, prepend=np.zeros((1, ch))) / q
    return out[:, 0] if y.ndim == 1 else out


# Integer PCM subtypes -> bit depth. Float sources (FLOAT/DOUBLE) carry more
# resolution than any integer target, so they always need requantisation.
_SUBTYPE_BITS = {"PCM_S8": 8, "PCM_U8": 8, "PCM_16": 16, "PCM_24": 24, "PCM_32": 32}


def source_bit_depth(path: str) -> Optional[int]:
    """Integer bit depth of the source file, or None for float / unknown."""
    try:
        return _SUBTYPE_BITS.get(sf.info(path).subtype)
    except Exception:
        return None


def _metrics(dry: np.ndarray, quant: np.ndarray) -> dict[str, float]:
    error = quant - dry
    rms = float(np.sqrt(np.mean(error ** 2)))
    noise_floor = round(20.0 * float(np.log10(rms + 1e-12)), 2)
    # Correlation of the error with the signal (mono-summed), magnitude
    # (the shared Pearson; 0 when either side has no variance).
    from khensu_services.analysis.dsp_metrics import pearson
    e = error.mean(axis=1) if error.ndim > 1 else error
    s = dry.mean(axis=1) if dry.ndim > 1 else dry
    corr = abs(pearson(e, s, silent=0.0))
    return {"noise_floor_dbfs": noise_floor, "distortion_correlation": round(corr, 3)}


def analyze_dither(src_path: str, target_bits: int = 16,
                   out_dir: Optional[str] = None, seed: int = 12345) -> dict[str, Any]:
    """Compare truncation vs TPDF dither vs shaped dither at target_bits, and
    render the dithered result for audition. Never raises on the DSP path."""
    from khensu_services.copilot.dsp import require_finite_audio
    dry, sr = sf.read(src_path, always_2d=True)
    require_finite_audio(dry, "dither source")  # audit CQ-01 (np.clip won't fix NaN)
    rng = np.random.default_rng(seed)

    # Metric window (capped so long files stay cheap).
    n = int(min(len(dry), _METRIC_MAX_SEC * sr))
    win = dry[:n]

    trunc = _quantize_vectorised(win, target_bits, dither=False, rng=rng, truncate=True)
    tpdf = _quantize_vectorised(win, target_bits, dither=True, rng=rng)
    shaped = _quantize_shaped(win, target_bits, rng=rng)

    modes = [
        {"mode": "truncate", "label": "Truncation (no dither)", **_metrics(win, trunc)},
        {"mode": "tpdf", "label": "TPDF dither", **_metrics(win, tpdf)},
        {"mode": "shaped", "label": "Dither + noise shaping", **_metrics(win, shaped)},
    ]

    # Audition render: full-length TPDF-dithered, written at target bit depth.
    out_dir = out_dir or os.path.join(os.path.dirname(src_path), "dither_preview")
    os.makedirs(out_dir, exist_ok=True)
    base = os.path.splitext(os.path.basename(src_path))[0]
    audition = os.path.join(out_dir, f"{base}_dither{target_bits}.wav")
    full = _quantize_vectorised(dry, target_bits, dither=True, rng=rng)
    subtype = {16: "PCM_16", 24: "PCM_24", 8: "PCM_U8"}.get(target_bits, "PCM_16")
    sf.write(audition, np.clip(full, -1.0, 1.0), sr, subtype=subtype)

    trunc_floor = modes[0]["noise_floor_dbfs"]
    tpdf_floor = modes[1]["noise_floor_dbfs"]
    src_bits = source_bit_depth(src_path)
    # Writing an N-bit source at >= N bits keeps every sample exactly: there is
    # no requantisation, so nothing for dither to decorrelate.
    dither_needed = src_bits is None or target_bits < src_bits
    if dither_needed:
        recommendation = (
            "Dither: truncation adds signal-correlated quantisation distortion "
            f"(correlation {modes[0]['distortion_correlation']}); TPDF/shaped "
            "dither replaces it with a benign noise floor "
            f"(correlation {modes[1]['distortion_correlation']}).")
    else:
        recommendation = (
            f"No dither needed: the source is already {src_bits}-bit, so writing it at "
            f"{target_bits}-bit is lossless - dither would only add noise.")
    return {
        "target_bits": target_bits,
        "source_bits": src_bits,
        "dither_needed": dither_needed,
        "sample_rate": sr,
        "modes": modes,
        "audition_path": audition,
        # Dither raises the noise floor slightly but decorrelates it — the whole
        # point. Surface both numbers so the trade-off is explicit.
        "noise_floor_delta_db": round(tpdf_floor - trunc_floor, 2),
        "recommendation": recommendation,
    }
