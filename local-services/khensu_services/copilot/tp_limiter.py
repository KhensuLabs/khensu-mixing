"""True-peak lookahead limiter (audit AUDIT-006).

pedalboard's ``Limiter`` is a maximiser, not a ceiling: it applies roughly
``+(3.75 - threshold)`` dB of makeup, a 4:1 stage and a hard clip at 0 dBFS,
so a render described as "limited to -1 dBTP" was neither at -1 dBTP nor at
its original level. This module is the ceiling every render path uses instead.

Algorithm (fully vectorised, no per-sample Python loop):

1. Peak detector: the 8x-oversampled envelope from ``analysis/loudness.py``
   (the true-peak meter's Kaiser interpolator, at twice the meter's rate),
   folded back to one value per input sample, so inter-sample peaks are seen.
   8x, not the meter's 4x (audit CPL-14): a 4x detector let the output sit
   +0.09 dB (music) to +0.28 dB (transient noise) over the ceiling on a 16x
   meter. At 8x the residual between 4x-sampled points is a few hundredths of
   a dB, so the ceiling holds on finer meters too, not just the 4x one.
2. Required gain reduction ``u[n] = max(0, 20*log10(peak[n] / ceiling))``.
3. Lookahead hold: ``m[n] = max(u[n .. n+L])`` (forward window of L+1).
4. Release: ``v[n] = max(m[n], a * v[n-1])`` with ``a = exp(-1/(t_r*fs))`` -
   an exponential decay of the reduction in dB, computed in closed form as a
   cumulative max in the log domain.
5. Attack: a length-L moving average of ``v`` ending at ``n``. Because every
   ``v[k]`` with ``k`` in ``[n-L, n]`` is at least ``u[n]``, the average is
   too, so the ceiling holds while the gain ramps smoothly over the lookahead.

The gain is applied to the *aligned* signal (the lookahead is expressed in the
gain curve rather than by delaying the audio), so the output has exactly the
input's length and timing: no latency to compensate downstream.

Gain changes are slow relative to the interpolation filter but not static,
so the measured true peak of ``gain * x`` can differ from ``gain * peak(x)``
by a few hundredths of a dB; the output is re-measured and any residue is
trimmed statically, so ``true_peak_dbtp(output) <= ceiling_dbtp``.
"""
from __future__ import annotations

import numpy as np

_OVERSAMPLE = 8
_MARGIN_DB = 0.05


def _per_sample_true_peak(x: np.ndarray) -> np.ndarray:
    """Per-input-sample peak of the oversampled signal (max over channels).

    Oversampled points ``8n .. 8n+7`` lie between samples ``n`` and ``n+1``;
    each sample takes the max of its own block and the previous one so an
    inter-sample peak is charged to both neighbours.
    """
    from khensu_services.analysis.loudness import oversampled_envelope

    n = x.shape[1]
    env = oversampled_envelope(x, _OVERSAMPLE).reshape(n, _OVERSAMPLE).max(axis=1)
    env = np.maximum(env, np.abs(x).max(axis=0))
    env[1:] = np.maximum(env[1:], env[:-1])
    return env


def _release_hold(m: np.ndarray, a: float) -> np.ndarray:
    """``v[n] = max(m[n], a * v[n-1])`` (v[-1] = 0) without a Python loop.

    Unrolled, ``v[n] = max_{j<=n} m[j] * a^(n-j)``; in the log domain that is
    ``n*log(a) + cummax(log m[j] - j*log(a))``, which never overflows.
    """
    la = float(np.log(a))
    idx = np.arange(m.shape[0], dtype=np.float64)
    with np.errstate(divide="ignore"):
        logm = np.log(m)
    acc = np.maximum.accumulate(logm - idx * la)
    with np.errstate(invalid="ignore"):
        v = np.exp(idx * la + acc)
    return np.nan_to_num(v, nan=0.0)


def _limit_once(x: np.ndarray, sr: int, ceiling_lin: float, lookahead: int,
                release_coeff: float) -> np.ndarray:
    from scipy.ndimage import maximum_filter1d

    n = x.shape[1]
    peak = _per_sample_true_peak(x)
    with np.errstate(divide="ignore"):
        u = np.maximum(0.0, 20.0 * np.log10(np.maximum(peak, 1e-30) / ceiling_lin))
    if not np.any(u > 0.0):
        return x

    L = lookahead
    # Extended timeline: L zeros before (so the attack ramp can start before
    # sample 0) and L after (the forward window reads past the end).
    up = np.concatenate([np.zeros(L), u, np.zeros(L)])
    m = maximum_filter1d(up, size=L + 1, origin=-((L + 1) // 2), mode="constant", cval=0.0)
    v = _release_hold(m, release_coeff)
    c = np.concatenate([[0.0], np.cumsum(v)])
    # Trailing mean over L samples ending at extended index j = i + L.
    j = np.arange(L, L + n)
    reduction_db = (c[j + 1] - c[j + 1 - L]) / L
    # Float rounding in the cumsum must never relax the guarantee.
    reduction_db = np.maximum(reduction_db, u)
    gain = 10.0 ** (-reduction_db / 20.0)
    return x * gain[np.newaxis, :]


def true_peak_limit(audio: np.ndarray, sr: int, ceiling_dbtp: float = -1.0,
                    lookahead_ms: float = 1.5, release_ms: float = 60.0) -> np.ndarray:
    """Limit ``audio`` so its true peak stays at ``ceiling_dbtp`` - measured at
    8x, so it also holds on the BS.1770 4x meter and on finer (16x) meters.

    ``audio`` is ``(channels, samples)`` (a 1-D mono buffer is accepted too);
    the gain is linked across channels so the stereo image never shifts.
    Returns the same shape and dtype, sample-aligned with the input. Material
    already below the ceiling is returned bit-for-bit unchanged. The limiter
    never adds gain: drive a quiet source into it with an explicit Gain.
    """
    from khensu_services.analysis.loudness import true_peak_linear

    arr = np.asarray(audio)
    if arr.size == 0:
        return arr.copy()
    was_1d = arr.ndim == 1
    x = arr[np.newaxis, :].astype(np.float64) if was_1d else arr.astype(np.float64)
    if not np.isfinite(x).all():
        raise ValueError("true_peak_limit: audio contains non-finite samples")

    # A 0.05 dB safety margin under the requested ceiling (CPL-14): with the
    # 8x detector the residual a 16x meter can still find between 8x points
    # is about that size on full-band noise.
    ceiling_lin = 10.0 ** ((float(ceiling_dbtp) - _MARGIN_DB) / 20.0)
    lookahead = max(1, int(round(float(lookahead_ms) * 1e-3 * sr)))
    release_coeff = float(np.exp(-1.0 / (max(1.0, float(release_ms)) * 1e-3 * sr)))

    y = _limit_once(x, sr, ceiling_lin, lookahead, release_coeff)
    if y is not x:
        tp = true_peak_linear(y, _OVERSAMPLE)
        if tp > ceiling_lin:
            # Residual overshoot from gain modulation (~0.01-0.05 dB): a
            # static trim that small is inaudible and makes the ceiling exact.
            y = y * (ceiling_lin / tp)

    if y is x:
        return arr.copy()
    out = y[0] if was_1d else y
    return out.astype(arr.dtype, copy=False)
