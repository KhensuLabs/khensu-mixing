"""Shared ITU-R BS.1770-4 / EBU R128 / Tech 3342 measurement core.

Every Python loudness / true-peak number in the service should come from here
so the analyzers agree with each other (and with the C++ LoudnessAnalyzer):

* ``integrated_lufs``   — BS.1770-4 gated integrated loudness. A mono signal is
  measured as ONE channel (G = 1). Copying mono to two channels before metering
  inflates the reading by +3.01 LU, which is what several analyzers used to do.
  5.1 / 7.1 (6 / 8 channels, SMPTE order) are weighted by layout: LFE 0, the
  60-120 degree surrounds 1.41, everything else 1.0, gated once across all
  channels.
* ``short_term_loudness_series`` — EBU Tech 3342 short-term loudness: the whole
  signal is K-weighted once, then UNGATED 3 s windows are taken at a 0.1 s hop
  and each is ``-0.691 + 10*log10(sum_i G_i * mean(z_i^2))``. (The old code ran
  pyloudnorm's *gated integrated* meter per window, which applies the BS.1770
  relative gate inside every 3 s block — not short-term loudness.) Exactly one
  value per window; digitally silent windows are NaN, never dropped, so the
  series stays time-aligned with other per-window curves.
* ``loudness_range``    — Tech 3342 LRA from that series: absolute gate -70
  LUFS, relative gate -20 LU below the energy mean of the absolute-gated
  blocks, LRA = P95 - P10. ``None`` for audio shorter than one 400 ms block.
* ``true_peak_dbtp``    — BS.1770-4 Annex 2-style true peak: 4x polyphase FIR
  oversampling (``scipy.signal.resample_poly`` with a 257-tap Kaiser-windowed
  sinc: passband flat within +/-0.0003 dB to 20 kHz at 44.1 kHz, >90 dB image
  rejection — tighter than the Annex 2 48-tap example filter) and zero
  padding at both ends. No circular FFT resampling, so the end of the file
  can't wrap around into the start and fake an inter-sample peak.
* ``crest_factor_db``   — peak/RMS in dB measured PER CHANNEL, reporting the
  largest channel crest; ``None`` when every channel is below a -90 dBFS RMS
  floor (the ratio of two numerical-noise values is meaningless).
* ``clip_events`` / ``flat_top_mask`` — the one clip detector: flat tops (3+
  samples at the channel's own peak that do not move), per channel.
* K-weighting is the ITU-R BS.1770-4 pre-filter + RLB high-pass with the
  exact reference coefficients at 48 kHz (``k_weighting_coefficients``);
  LRA percentiles are nearest-rank like the C++ meter.

Array convention: ``(channels, samples)`` or 1-D mono. Callers holding
``(frames, channels)`` buffers (soundfile) pass ``y.T``.
"""
from __future__ import annotations

import functools
import warnings
from typing import Optional

import numpy as np
import numpy.typing as npt

# BS.1770-4 channel weights for L, R, C, Ls, Rs.
_CHANNEL_GAINS = (1.0, 1.0, 1.0, 1.41, 1.41)
# Surround layouts, identified by channel count in SMPTE order (the same table
# as common/channel_layout.py): 5.1 = L R C LFE Ls Rs, 7.1 = L R C LFE Ls Rs Lb
# Rb. BS.1770-4 excludes LFE (weight 0) and weights by position (Table 4):
# surrounds at 60-120 deg azimuth (5.1 Ls/Rs at ~110 deg, 7.1 side Ls/Rs at
# ~90 deg) take 1.41; everything else - including 7.1 rear Lb/Rb at ~135-150
# deg - takes 1.0. (AUDIT-035)
_SURROUND_GAINS: dict[int, tuple[float, ...]] = {
    6: (1.0, 1.0, 1.0, 0.0, 1.41, 1.41),
    8: (1.0, 1.0, 1.0, 0.0, 1.41, 1.41, 1.0, 1.0),
}
_BLOCK_400MS = 0.4
ABSOLUTE_GATE_LUFS = -70.0
RELATIVE_GATE_LU = -20.0
# Below this RMS a crest factor is the ratio of two noise-level numbers.
CREST_RMS_FLOOR_DBFS = -90.0
# Samples of real context carried into each true-peak chunk on both sides —
# longer than the interpolation filter's half span (32 input samples), so
# chunked output equals whole-signal output.
_TP_CHUNK = 1 << 18
_TP_CONTEXT = 64
_TP_TAPS_PER_PHASE = 64       # 257 taps at 4x
_TP_KAISER_BETA = 9.0
_tp_filters: dict[int, np.ndarray] = {}
# K-weighting runs in chunks of this many samples, carrying the IIR state
# across chunk boundaries (``lfilter(..., zi=...)``), so the output is
# bit-identical to filtering the whole signal while a long file never needs
# float64 copies of every channel at once (2026-11-06 intake memory pass).
_KW_CHUNK = 1 << 18


def _tp_filter(oversample: int) -> np.ndarray:
    """Linear-phase interpolation low-pass (cutoff = input Nyquist), cached."""
    h = _tp_filters.get(oversample)
    if h is None:
        from scipy.signal import firwin
        n_taps = _TP_TAPS_PER_PHASE * oversample + 1
        h = firwin(n_taps, 1.0 / oversample, window=("kaiser", _TP_KAISER_BETA))
        _tp_filters[oversample] = h
    return h


def as_channels(y: npt.ArrayLike) -> np.ndarray:
    """Return a float64 ``(channels, samples)`` view of a 1-D or 2-D array."""
    arr = np.asarray(y, dtype=np.float64)
    if arr.ndim == 1:
        return arr[np.newaxis, :]
    return arr


def _channels_any_dtype(y: npt.ArrayLike) -> np.ndarray:
    """``(channels, samples)`` view WITHOUT the float64 upcast: a float32
    buffer stays float32 (no full-signal copy); callers convert per channel or
    per chunk, which yields exactly the values ``as_channels`` would have."""
    arr = np.asarray(y)
    if arr.ndim == 1:
        return arr[np.newaxis, :]
    return arr


# ITU-R BS.1770-4 Tables 1 and 2: the K-weighting pre-filter (high shelf) and
# the RLB high-pass at 48 kHz, verbatim. The RLB numerator is exactly
# [1, -2, 1] - NOT the RBJ-cookbook high-pass, whose (1 + cos w0) / 2
# numerator normalises the passband to unity and reads ~0.04 LU low against
# the reference (pyloudnorm's default "K-weighting" class does that, and also
# uses 38 Hz / a 1500 Hz shelf). (CORE-12)
_ITU_48K_SHELF = ((1.53512485958697, -2.69169618940638, 1.19839281085285),
                  (1.0, -1.69065929318241, 0.73248077421585))
_ITU_48K_RLB = ((1.0, -2.0, 1.0),
                (1.0, -1.99004745483398, 0.99007225036621))
# Analog-prototype parameters that reproduce those tables (Brecht De Man's
# derivation, the same numbers as pyloudnorm's "DeMan" class): at 48 kHz the
# bilinear designs below match the ITU coefficients to ~1e-12, and they
# re-derive the filters at every other sample rate.
_SHELF_G_DB = 3.99984385397
_SHELF_Q = 0.7071752369554193
_SHELF_FC = 1681.9744509555319
_RLB_Q = 0.5003270373253953
_RLB_FC = 38.13547087613982


def _design_shelf(sr: int) -> tuple[np.ndarray, np.ndarray]:
    K = np.tan(np.pi * _SHELF_FC / sr)
    Vh = np.power(10.0, _SHELF_G_DB / 20.0)
    Vb = np.power(Vh, 0.499666774155)
    a0_ = 1.0 + K / _SHELF_Q + K * K
    b0 = (Vh + Vb * K / _SHELF_Q + K * K) / a0_
    b1 = 2.0 * (K * K - Vh) / a0_
    b2 = (Vh - Vb * K / _SHELF_Q + K * K) / a0_
    a1 = 2.0 * (K * K - 1.0) / a0_
    a2 = (1.0 - K / _SHELF_Q + K * K) / a0_
    return np.array([b0, b1, b2]) / 1.0, np.array([1.0, a1, a2]) / 1.0


def _design_rlb(sr: int) -> tuple[np.ndarray, np.ndarray]:
    K = np.tan(np.pi * _RLB_FC / sr)
    a1 = 2.0 * (K * K - 1.0) / (1.0 + K / _RLB_Q + K * K)
    a2 = (1.0 - K / _RLB_Q + K * K) / (1.0 + K / _RLB_Q + K * K)
    return np.array([1.0, -2.0, 1.0]) / 1.0, np.array([1.0, a1, a2]) / 1.0


@functools.lru_cache(maxsize=16)
def k_weighting_coefficients(sr: int) -> tuple[tuple[np.ndarray, np.ndarray], ...]:
    """``((b_shelf, a_shelf), (b_rlb, a_rlb))`` for ``sr``.

    48 kHz returns the ITU-R BS.1770-4 table values exactly; other rates use
    the bilinear designs that reproduce that table at 48 kHz."""
    sr = int(sr)
    if sr == 48000:
        return tuple((np.array(b, dtype=np.float64), np.array(a, dtype=np.float64))
                     for b, a in (_ITU_48K_SHELF, _ITU_48K_RLB))
    return (_design_shelf(sr), _design_rlb(sr))


def _k_weight_chunks(ch: np.ndarray, sr: int):
    """Yield ``(start, k_weighted_chunk)`` for one channel, in order.

    The chunks concatenate to exactly ``k_weight(ch, sr)[0]``: the BS.1770
    high shelf, then the RLB high-pass (``k_weighting_coefficients``), with
    every stage's filter state carried across chunks."""
    import scipy.signal as sig

    stages = k_weighting_coefficients(int(sr))   # high shelf, then RLB high-pass
    states = [np.zeros(max(len(a), len(b)) - 1) for b, a in stages]
    n = ch.shape[0]
    for start in range(0, n, _KW_CHUNK):
        seg = np.asarray(ch[start:start + _KW_CHUNK], dtype=np.float64)
        for k, (b, a) in enumerate(stages):
            seg, states[k] = sig.lfilter(b, a, seg, zi=states[k])
        yield start, seg


def _k_weighted_channel(ch: np.ndarray, sr: int) -> np.ndarray:
    """K-weighted float64 copy of one channel (see ``_k_weight_chunks``)."""
    out = np.empty(ch.shape[0], dtype=np.float64)
    for start, seg in _k_weight_chunks(ch, sr):
        out[start:start + seg.shape[0]] = seg
    return out


def _weighted_kw_power(x: np.ndarray, sr: int, gains: np.ndarray) -> np.ndarray:
    """``sum_c gains[c] * kw_c**2`` per sample - the same values as
    ``np.einsum("c,cn->n", gains, kw * kw)`` - accumulated channel by channel
    and chunk by chunk, so the only full-length buffer is the result."""
    power = np.zeros(x.shape[1], dtype=np.float64)
    for c in range(x.shape[0]):
        g = gains[c]
        for start, seg in _k_weight_chunks(x[c], sr):
            np.multiply(seg, seg, out=seg)
            np.multiply(g, seg, out=seg)
            dst = power[start:start + seg.shape[0]]
            np.add(dst, seg, out=dst)
    return power


def _prefix_sum(v: np.ndarray, dtype=np.float64) -> np.ndarray:
    """``np.concatenate([[0], np.cumsum(v)])`` without the extra full copy."""
    out = np.empty(v.shape[0] + 1, dtype=dtype)
    out[0] = 0
    np.cumsum(v, out=out[1:], dtype=dtype)
    return out


def _channel_gains(n_channels: int) -> np.ndarray:
    if n_channels in _SURROUND_GAINS:
        return np.asarray(_SURROUND_GAINS[n_channels], dtype=np.float64)
    if n_channels > len(_CHANNEL_GAINS):
        raise ValueError(
            f"BS.1770 metering supports 1-5 channels, 5.1 (6) or 7.1 (8); got {n_channels}")
    return np.asarray(_CHANNEL_GAINS[:n_channels], dtype=np.float64)


def _gated_integrated(x: np.ndarray, sr: int, gains: np.ndarray) -> Optional[float]:
    """BS.1770-4 gated integrated loudness with explicit channel weights.

    400 ms blocks at a 100 ms step; the channel-weighted block power is gated
    once for all channels together (absolute -70 LUFS, then relative -10 LU
    below the absolute-gated mean). Used for layouts pyloudnorm cannot weight
    (5.1/7.1 with LFE excluded).
    """
    return gated_integrated_from_power(_weighted_kw_power(x, sr, gains), sr)


def kw_power(channel: npt.ArrayLike, sr: int) -> np.ndarray:
    """Squared K-weighted samples of ONE channel (float64, full length) - the
    per-channel term ``kw_c**2`` of ``_weighted_kw_power``. Callers metering a
    layout channel by channel (``analysis/immersive.py``: 7.1.4 and other
    layouts read one channel at a time to bound memory) add ``gain_c *
    kw_power(x_c, sr)`` themselves and pass the sum to
    ``gated_integrated_from_power``."""
    ch = np.asarray(channel)
    out = np.empty(ch.shape[0], dtype=np.float64)
    for start, seg in _k_weight_chunks(ch, int(sr)):
        np.multiply(seg, seg, out=seg)
        out[start:start + seg.shape[0]] = seg
    return out


def gated_integrated_from_power(power: np.ndarray, sr: int) -> Optional[float]:
    """BS.1770-4 gated integrated loudness from a channel-weighted K-weighted
    power signal ``sum_c G_c * kw_c**2`` (the surround-layout path of
    ``integrated_lufs``; ``None`` below one block or when fully gated).

    400 ms blocks at a 100 ms step; the channel-weighted block power is gated
    once for all channels together (absolute -70 LUFS, then relative -10 LU
    below the absolute-gated mean). Consumes ``power`` (it may be freed)."""
    sr = int(sr)
    block = int(round(_BLOCK_400MS * sr))
    hop = max(1, int(round(0.1 * sr)))
    n = power.shape[0]
    if n < block:
        return None
    csum = _prefix_sum(power)
    del power
    starts = np.arange(0, n - block + 1, hop)
    z = np.maximum((csum[starts + block] - csum[starts]) / block, 0.0)
    with np.errstate(divide="ignore"):
        lj = -0.691 + 10.0 * np.log10(z)
    abs_ok = lj > ABSOLUTE_GATE_LUFS
    if not np.any(abs_ok):
        return None
    rel = -0.691 + 10.0 * np.log10(np.mean(z[abs_ok])) - 10.0
    keep = abs_ok & (lj > rel)
    if not np.any(keep):
        return None
    val = float(-0.691 + 10.0 * np.log10(np.mean(z[keep])))
    return val if np.isfinite(val) else None


def k_weight(y: npt.ArrayLike, sr: int) -> np.ndarray:
    """K-weighted copy of ``y`` as ``(channels, samples)`` (the BS.1770
    pre-filter + RLB high-pass of ``k_weighting_coefficients(sr)``)."""
    x = _channels_any_dtype(y)
    out = np.empty(x.shape, dtype=np.float64)
    for c in range(x.shape[0]):
        out[c] = _k_weighted_channel(x[c], int(sr))
    return out


def _pyloudnorm_integrated(x: np.ndarray, sr: int) -> float:
    """``pyloudnorm.Meter(sr, filter_class="DeMan").integrated_loudness(x.T)``
    (the ITU-exact K-weighting), one channel at a time.

    pyloudnorm copies the whole input and keeps every K-weighted channel alive
    at once (several full-length float64 buffers for a long file). This
    repeats its exact arithmetic - the same block bounds, the same per-block
    ``np.sum(np.square(...))`` and the same gating lists - but holds a single
    K-weighted channel at a time, so the result is the same float (at 48 kHz
    the filters are the ITU table itself, which differs from the "DeMan"
    design by ~1e-12).
    """
    rate = int(sr)
    t_g = 0.400
    step = 1.0 - 0.75
    gains = [1.0, 1.0, 1.0, 1.41, 1.41]
    gamma_a = -70.0
    num_channels, num_samples = x.shape
    t_len = num_samples / rate
    num_blocks = int(np.round(((t_len - t_g) / (t_g * step)))+1)
    bounds = [(int(t_g * (j * step) * rate), int(t_g * (j * step + 1) * rate))
              for j in range(num_blocks)]
    z = np.zeros(shape=(num_channels, num_blocks))
    for i in range(num_channels):
        kw = _k_weighted_channel(x[i], sr)
        for j, (lo, hi) in enumerate(bounds):
            z[i, j] = (1.0 / (t_g * rate)) * np.sum(np.square(kw[lo:hi]))
        del kw
    with warnings.catch_warnings(), np.errstate(divide="ignore", invalid="ignore"):
        warnings.simplefilter("ignore", category=RuntimeWarning)
        lj = [-0.691 + 10.0 * np.log10(np.sum([gains[i] * z[i, j] for i in range(num_channels)]))
              for j in range(num_blocks)]
        j_g = [j for j, l_j in enumerate(lj) if l_j >= gamma_a]
        z_avg = [np.mean([z[i, j] for j in j_g]) for i in range(num_channels)]
        gamma_r = -0.691 + 10.0 * np.log10(np.sum([gains[i] * z_avg[i] for i in range(num_channels)])) - 10.0
        j_g = [j for j, l_j in enumerate(lj) if (l_j > gamma_r and l_j > gamma_a)]
        z_avg = np.nan_to_num(np.array([np.mean([z[i, j] for j in j_g]) for i in range(num_channels)]))
        return float(-0.691 + 10.0 * np.log10(np.sum([gains[i] * z_avg[i] for i in range(num_channels)])))


def integrated_lufs(y: npt.ArrayLike, sr: int) -> Optional[float]:
    """BS.1770-4 gated integrated loudness in LUFS.

    1-D input is ONE channel (G = 1) — not duplicated to stereo. Returns
    ``None`` for audio shorter than one 400 ms gating block or with no block
    above the absolute gate (silence), instead of raising / returning -inf.
    """
    x = _channels_any_dtype(y)
    # Exactly one 400 ms block is measurable (it used to need one sample
    # more, so a 400 ms clip read None while 401 ms read a value). (DSP-12)
    if x.size == 0 or sr <= 0 or x.shape[1] < int(round(_BLOCK_400MS * sr)):
        return None
    gains = _channel_gains(x.shape[0])   # validates the channel count
    if x.shape[0] in _SURROUND_GAINS:
        return _gated_integrated(x, int(sr), gains)
    val = _pyloudnorm_integrated(x, int(sr))
    return val if np.isfinite(val) else None


def short_term_loudness_series(y: npt.ArrayLike, sr: int, block_sec: float = 3.0,
                               hop_sec: float = 0.1) -> np.ndarray:
    """Ungated short-term (``block_sec``) loudness, one value per ``hop_sec``.

    Window k spans ``[k*hop, k*hop + block)``; only full windows are emitted.
    A signal shorter than one block yields a single value over everything.
    Windows that are digitally silent in the input (every sample 0) are NaN,
    kept in place so the series stays time-aligned.
    """
    x = _channels_any_dtype(y)
    n = x.shape[1]
    if n == 0 or sr <= 0:
        return np.array([], dtype=np.float64)
    gains = _channel_gains(x.shape[0])
    # Channel-weighted instantaneous power, then window means via a cumsum.
    power = _weighted_kw_power(x, int(sr), gains)
    block = max(1, int(round(block_sec * sr)))
    hop = max(1, int(round(hop_sec * sr)))
    # Digital silence is judged on the INPUT: the K-weighting IIR rings on
    # after content stops, so a silent window would otherwise read a finite
    # (meaningless) -60..-120 LUFS from the previous window's filter tail.
    nonzero = np.zeros(n, dtype=bool)
    for start in range(0, n, _KW_CHUNK):
        nonzero[start:start + _KW_CHUNK] = np.any(x[:, start:start + _KW_CHUNK] != 0.0, axis=0)
    if n < block:
        means = np.array([float(np.mean(power))])
        live = np.array([nonzero.any()])
    else:
        csum = _prefix_sum(power)
        del power
        # Integer counts: exact, as the float64 cumsum of 0/1 was. Built in
        # chunks so the bool -> int cast never materializes full-length.
        count_dtype = np.int32 if n < np.iinfo(np.int32).max else np.int64
        nz_sum = np.empty(n + 1, dtype=count_dtype)
        nz_sum[0] = 0
        running = 0
        for start in range(0, n, _KW_CHUNK):
            part = nz_sum[start + 1:start + 1 + _KW_CHUNK]
            np.cumsum(nonzero[start:start + _KW_CHUNK], out=part, dtype=count_dtype)
            part += running
            running = int(part[-1])
        del nonzero
        starts = np.arange(0, n - block + 1, hop)
        means = (csum[starts + block] - csum[starts]) / block
        live = (nz_sum[starts + block] - nz_sum[starts]) > 0.5
    means = np.maximum(means, 0.0)   # cumsum rounding can go fractionally < 0
    with np.errstate(divide="ignore"):
        out = -0.691 + 10.0 * np.log10(means)
    out[~np.isfinite(out) | ~live] = np.nan
    return out


def loudness_range(y: npt.ArrayLike, sr: int) -> Optional[float]:
    """EBU Tech 3342 loudness range (LU), or ``None`` for audio < 400 ms.

    1. Short-term loudness series (3 s ungated windows, 0.1 s hop).
    2. Absolute gate: keep blocks >= -70 LUFS.
    3. Relative gate: keep blocks >= (energy mean of step 2) - 20 LU.
    4. LRA = P95 - P10 of the survivors (0.0 if fewer than two survive),
       nearest-rank on the ``(n - 1)`` basis - the same percentile rule as
       the C++ LoudnessAnalyzer (``sorted[lround(p * (n - 1))]``), so the two
       meters agree (CORE-12; numpy's default is linear interpolation).
    """
    x = _channels_any_dtype(y)
    if x.size == 0 or sr <= 0 or x.shape[1] < int(round(_BLOCK_400MS * sr)):
        return None
    st = short_term_loudness_series(x, sr)
    st = st[np.isfinite(st)]
    abs_gated = st[st >= ABSOLUTE_GATE_LUFS]
    if abs_gated.size < 2:
        return 0.0
    rel_thresh = 10.0 * np.log10(np.mean(10.0 ** (abs_gated / 10.0))) + RELATIVE_GATE_LU
    rel_gated = abs_gated[abs_gated >= rel_thresh]
    if rel_gated.size < 2:
        return 0.0
    return max(0.0, _nearest_rank(rel_gated, 0.95) - _nearest_rank(rel_gated, 0.10))


def _nearest_rank(values: np.ndarray, p: float) -> float:
    """``sorted(values)[round_half_away(p * (n - 1))]`` - C++ ``std::lround``."""
    s = np.sort(np.asarray(values, dtype=np.float64))
    idx = int(np.floor(p * (s.shape[0] - 1) + 0.5))
    return float(s[min(max(idx, 0), s.shape[0] - 1)])


def crest_factor_db(x: npt.ArrayLike) -> Optional[float]:
    """Sample-peak / RMS crest factor in dB, measured per channel.

    ``x`` is ``(channels, samples)`` or 1-D mono. Each channel's crest is its
    own sample peak over its own RMS, and the LARGEST channel crest is
    reported. Channels whose RMS is at or below -90 dBFS are skipped (their
    ratio is noise over noise); ``None`` when no channel qualifies or there is
    no audio.

    Why per channel, and why the max: a mono downmix ``(L + R) / 2`` is not the
    signal on either channel — anti-phase or hard-panned content partly
    cancels and peaks that land on one side are averaged down, so the downmix
    crest can disagree with what a per-channel meter shows by several dB.
    Taking the max mirrors how sample/true-peak are reported (the worst
    channel), so a single dynamic channel is never hidden by a dense one. For
    a mono file, or stereo with identical channels, the result equals the old
    single-channel value.
    """
    ch = _channels_any_dtype(x)
    if ch.size == 0:
        return None
    floor = 10.0 ** (CREST_RMS_FLOOR_DBFS / 20.0)
    # One float64 channel at a time (not the whole signal plus its square).
    rms = np.empty(ch.shape[0])
    peak = np.empty(ch.shape[0])
    for c in range(ch.shape[0]):
        row = np.array(ch[c], dtype=np.float64)       # always a private copy
        # max|x| == max(max x, -min x) exactly, without an |x| temporary.
        peak[c] = max(float(np.max(row)), -float(np.min(row)))
        np.multiply(row, row, out=row)                 # squared in place
        rms[c] = np.sqrt(np.mean(row))
        del row
    valid = rms > floor
    if not np.any(valid):
        return None
    return float(np.max(20.0 * np.log10(peak[valid] / rms[valid])))


# ---------------------------------------------------------------------------
# Clipping (flat-top detector) - the ONE clip definition (DSP-01, S10)
# ---------------------------------------------------------------------------

# A clip is a FLAT TOP: at least this many consecutive samples that sit at the
# channel's own peak (within CLIP_NEAR_PEAK of it) AND do not move (each step
# smaller than CLIP_FLAT_TOL x the peak). Near the crest of an unclipped
# low-frequency sine at 0 dBFS neighbouring samples are within 0.1 % of full
# scale, so the old "2+ samples >= 0.999" rule read a clean 50 Hz master as
# 500 clip events; those samples still move, a truncated waveform does not.
# Measured against the channel's own peak, so a mix clipped and then turned
# down is still caught.
CLIP_MIN_RUN = 3
CLIP_NEAR_PEAK = 0.999
CLIP_FLAT_TOL = 1e-6
# ...and the waveform must step onto or off the plateau by at least this much
# (x peak): ~3 LSB at 16 bits (see flat_top_mask).
CLIP_EDGE_STEP = 1e-4
# Channels peaking below this (-20 dBFS) are not examined: a flat top there is
# a quiet square-ish waveform, not converter / limiter truncation.
CLIP_MIN_PEAK = 0.1


def _true_runs(mask: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """(starts, ends) of the True runs in a 1-D boolean mask (ends exclusive)."""
    if mask.size == 0 or not mask.any():
        empty = np.zeros(0, dtype=np.int64)
        return empty, empty
    edges = np.diff(mask.astype(np.int8))
    starts = np.flatnonzero(edges == 1) + 1
    ends = np.flatnonzero(edges == -1) + 1
    if mask[0]:
        starts = np.concatenate([[0], starts])
    if mask[-1]:
        ends = np.concatenate([ends, [mask.shape[0]]])
    return starts, ends


def flat_top_mask(channel: npt.ArrayLike, min_run: int = CLIP_MIN_RUN) -> np.ndarray:
    """Boolean mask of the samples of ONE channel that belong to a flat-top
    clip run (see ``CLIP_MIN_RUN``)."""
    ch = np.asarray(channel)
    n = ch.shape[0]
    out = np.zeros(n, dtype=bool)
    if n < max(2, min_run):
        return out
    peak = max(float(np.max(ch)), -float(np.min(ch)))
    if not np.isfinite(peak) or peak < CLIP_MIN_PEAK:
        return out
    near = np.abs(ch) >= CLIP_NEAR_PEAK * peak
    if not near.any():
        return out
    # pair[i]: samples i and i+1 are both at the peak and equal (same sign).
    step = np.abs(np.diff(np.asarray(ch, dtype=np.float64)))
    pair = near[:-1] & near[1:] & (step <= CLIP_FLAT_TOL * peak)
    starts, ends = _true_runs(pair)          # sample run = [s, e] inclusive
    if starts.size == 0:
        return out
    keep = (ends - starts) + 1 >= min_run    # k flat pairs = k + 1 samples
    # A truncated waveform arrives at (and leaves) the plateau with a slope;
    # the crest of a smooth sub-bass wave quantised to 16 bits can also sit on
    # 3-4 equal samples but steps onto them by ~1 LSB. So at least one edge
    # of the run must step by CLIP_EDGE_STEP x peak (a run touching both file
    # ends has no edge and counts).
    m = step.shape[0]
    edge_in = np.where(starts > 0, step[np.maximum(starts - 1, 0)], -np.inf)
    edge_out = np.where(ends < m, step[np.minimum(ends, m - 1)], -np.inf)
    no_edges = (starts == 0) & (ends >= m)
    keep &= no_edges | (np.maximum(edge_in, edge_out) >= CLIP_EDGE_STEP * peak)
    for s0, e0 in zip(starts[keep], ends[keep]):
        out[s0:e0 + 1] = True
    return out


def clip_events(y: npt.ArrayLike, max_positions: int = 10) -> tuple[int, list[float]]:
    """Flat-top clip events, per channel, OR-ed on the time axis.

    Clipping is a per-channel property (a flat-topped left channel can hide
    in a mono downmix), so each channel is examined on its own and a clip
    hitting both channels at once counts as ONE event. Returns
    ``(event_count, first max_positions event starts as fractions 0..1)``.
    """
    x = _channels_any_dtype(y)
    n = x.shape[1] if x.ndim == 2 else 0
    if n == 0:
        return 0, []
    clipped = np.zeros(n, dtype=bool)
    for c in range(x.shape[0]):
        clipped |= flat_top_mask(x[c])
    event_starts, _ = _true_runs(clipped)
    positions = (event_starts[:max_positions] / max(1, n)).round(4).tolist()
    return int(event_starts.shape[0]), [float(p) for p in positions]


# ---------------------------------------------------------------------------
# True peak (BS.1770-4 Annex 2 style)
# ---------------------------------------------------------------------------

def _oversampled_chunks(ch: np.ndarray, oversample: int):
    """Yield ``(out_start, upsampled_chunk)`` covering the whole channel.

    Each chunk is interpolated with real neighbouring samples as context and
    zeros only beyond the true ends of the signal, so concatenating the chunks
    reproduces ``resample_poly(ch, oversample, 1)`` exactly while bounding
    memory on long files.
    """
    import scipy.signal as sig

    n = ch.shape[0]
    for s in range(0, n, _TP_CHUNK):
        e = min(n, s + _TP_CHUNK)
        lo, hi = max(0, s - _TP_CONTEXT), min(n, e + _TP_CONTEXT)
        seg = np.asarray(ch[lo:hi], dtype=np.float64)   # per chunk, not per file
        pad_l, pad_r = _TP_CONTEXT - (s - lo), _TP_CONTEXT - (hi - e)
        if pad_l or pad_r:   # explicit zeros at the true signal ends
            seg = np.concatenate([np.zeros(pad_l), seg, np.zeros(pad_r)])
        up = sig.resample_poly(seg, oversample, 1, window=_tp_filter(oversample),
                               padtype="constant")
        k0 = _TP_CONTEXT * oversample
        yield s * oversample, up[k0:k0 + (e - s) * oversample]


def true_peak_per_channel_linear(y: npt.ArrayLike, oversample: int = 4) -> np.ndarray:
    """Linear true peak per channel (max |x| of the 4x-oversampled signal,
    never below the sample peak)."""
    x = _channels_any_dtype(y)
    peaks = np.zeros(x.shape[0])
    for c in range(x.shape[0]):
        if x.shape[1] == 0:
            continue
        # max|x| == max(max x, -min x) exactly, without an |x| temporary.
        p = max(float(np.max(x[c])), -float(np.min(x[c])))
        for _, up in _oversampled_chunks(x[c], oversample):
            if up.size:
                p = max(p, float(np.max(np.abs(up))))
        peaks[c] = p
    return peaks


def true_peak_linear(y: npt.ArrayLike, oversample: int = 4) -> float:
    """Linear true peak over all channels (0.0 for empty input)."""
    peaks = true_peak_per_channel_linear(y, oversample)
    return float(np.max(peaks)) if peaks.size else 0.0


def true_peak_dbtp(y: npt.ArrayLike, sr: int | None = None, oversample: int = 4) -> float:
    """True peak in dBTP over all channels (``sr`` accepted for API symmetry;
    the 4x interpolation is rate-independent)."""
    return float(20.0 * np.log10(true_peak_linear(y, oversample) + 1e-12))


def iter_oversampled_envelope(y: npt.ArrayLike, oversample: int = 4):
    """Yield ``(out_start, env_chunk)``: :func:`oversampled_envelope` one chunk
    at a time (every channel's chunks share boundaries), so a caller that
    only needs the overs never holds the n * oversample array - 366 MB for a
    4:20 stereo track (2026-11-18 stress test)."""
    x = _channels_any_dtype(y)
    gens = [_oversampled_chunks(x[c], oversample) for c in range(x.shape[0])]
    for parts in zip(*gens):
        start = parts[0][0]
        env = np.abs(parts[0][1])
        for _s, up in parts[1:]:
            np.maximum(env, np.abs(up), out=env)
        yield start, env


def oversampled_envelope(y: npt.ArrayLike, oversample: int = 4) -> np.ndarray:
    """``max_c |x_c|`` of the oversampled signal, full length (n * oversample)."""
    x = _channels_any_dtype(y)
    env = np.zeros(x.shape[1] * oversample)
    for start, chunk in iter_oversampled_envelope(x, oversample):
        env[start:start + chunk.shape[0]] = chunk
    return env


# ---------------------------------------------------------------------------
# Curves for the Dashboard loudness history view
# ---------------------------------------------------------------------------

def _windowed_crest_curve(y_stereo: np.ndarray, sr: int,
                          block_sec: float, hop_sec: float) -> list[Optional[float]]:
    """Peak/RMS crest factor (dB) per window, same windowing as the
    short-term loudness series — a rolling companion to the whole-track
    crest so where dynamics get squashed is visible. Per-channel, max
    across channels — the same definition as ``crest_factor_db``. ``None`` for
    windows below the -90 dBFS RMS floor."""
    x = _channels_any_dtype(y_stereo)
    n = x.shape[1]
    block = max(1, int(round(block_sec * sr)))
    hop = max(1, int(round(hop_sec * sr)))
    if n < block:
        return [crest_factor_db(x)]
    return [crest_factor_db(x[:, s:s + block])
            for s in range(0, n - block + 1, hop)]


def short_term_loudness_curve(y_stereo: np.ndarray, sr: int,
                              block_sec: float = 3.0, hop_sec: float = 1.0) -> dict:
    """Short-term loudness + rolling crest curve + true-peak overshoots for
    the Dashboard's loudness-history view (default 1 s display hop).

    Silent windows are ``None`` in both curves (kept in place, so the curve
    stays one-value-per-window and time-aligned) - the same convention as
    ``/analyze/metric_timeline``. They used to read -70 LUFS and 0 dB crest,
    and a 0 dB crest looks "fully squashed". (DSP-13)
    """
    x = _channels_any_dtype(y_stereo)
    curve = short_term_loudness_series(x, sr, block_sec, hop_sec)
    crest_curve = _windowed_crest_curve(x, sr, block_sec, hop_sec)
    duration = x.shape[1] / sr if sr > 0 else 0.0

    # True-peak overshoot distribution (Gemini audit): where inter-sample
    # peaks exceed the -1 dBTP ceiling, for the loudness-timeline overlay.
    from khensu_services.analysis.signal_integrity import true_peak_events
    tp = true_peak_events(x, sr)

    return {
        "duration_sec": float(duration),
        "hop_sec": float(hop_sec),
        "block_sec": float(block_sec),
        "lufs_curve": [float(v) if np.isfinite(v) else None for v in curve]
        if curve.size else [None],
        "crest_curve": [float(v) if v is not None else None for v in crest_curve],
        "true_peak_threshold_dbtp": tp["threshold_dbtp"],
        "true_peak_over_count": tp["over_count"],
        "true_peak_events": tp["events"],
    }
