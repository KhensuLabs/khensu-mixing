"""Dynamics processors: de-essers, compressors, gates, transient shaper, dynamic EQ, clipper, limiter (ENG-6, from dsp.py)."""
from __future__ import annotations

import numpy as np
from khensu_services.copilot.tp_limiter import true_peak_limit
from khensu_services.copilot.dsp_filters import _band_split


# ---------------------------------------------------------------------------
# De-esser — sidechain-EQ'd band compression (not a broadband compressor).
#
# Splits out the sibilance band with a zero-phase band-pass (so subtracting and
# re-adding the band introduces no comb filtering), runs a downward envelope
# compressor on the band only, and recombines:  y_out = (y - band) + gr * band.
# ---------------------------------------------------------------------------
def _apply_deesser(audio: np.ndarray, sr: int,
                   threshold_db: float = -30.0,
                   ratio: float = 4.0,
                   frequency_hz: float = 6500.0) -> np.ndarray:
    from scipy.signal import butter, sosfiltfilt, lfilter

    nyq = sr * 0.5
    lo = max(2000.0, frequency_hz / 1.6)
    hi = min(nyq * 0.95, frequency_hz * 1.6)
    if lo >= hi:
        return audio  # sample rate too low for the requested band

    sos = butter(4, [lo / nyq, hi / nyq], btype="band", output="sos")

    was_1d = audio.ndim == 1
    x = audio[np.newaxis, :] if was_1d else audio
    out = np.empty_like(x)

    # Vectorised one-pole envelope (~8 ms smoothing on the rectified band).
    # A signal-dependent attack/release follower needs a per-sample loop, which
    # is prohibitively slow in Python for full tracks; a short symmetric time
    # constant tracks sibilant bursts (typically 40-120 ms) closely enough.
    a = float(np.exp(-1.0 / (0.008 * sr)))
    thresh_lin = 10.0 ** (threshold_db / 20.0)
    inv_ratio = 1.0 / max(1.0, ratio)

    for c in range(x.shape[0]):
        band = sosfiltfilt(sos, x[c])
        env = lfilter([1.0 - a], [1.0, -a], np.abs(band))

        # Downward gain reduction above threshold: out_db = th + (in_db - th)/ratio
        with np.errstate(divide="ignore"):
            over = env / thresh_lin
        gr = np.where(over > 1.0, over ** (inv_ratio - 1.0), 1.0)

        out[c] = (x[c] - band) + band * gr

    return out[0] if was_1d else out


# ---------------------------------------------------------------------------
# Dynamic (program-dependent) de-esser — same isolate/duck/recombine structure
# as _apply_deesser, but the detector is the sibilance band's level RELATIVE
# to the whole signal rather than an absolute threshold. An absolute threshold
# has to be re-tuned per song section: set for the chorus it misses every
# quiet-verse ess, set for the verse it chews the chorus. Keying off the
# band-to-broadband ratio means "the S is hot relative to the voice right
# now", which is what an engineer actually rides — so one setting tracks the
# whole performance. rel_threshold_db is how far below the broadband level
# the sibilance band may sit before reduction starts (≈ -12 dB is a natural
# vocal's ceiling); max_reduction_db keeps extreme settings from lisping.
# ---------------------------------------------------------------------------
# Presence-gate window for the program-relative detector, in linear envelope
# amplitude. Below _LO the detector is fully muted (a filtfilt edge transient
# divided by near-silence is meaningless); above _HI it is fully live. Between
# them it ramps, so a signal hovering at the boundary cannot chatter the band
# gain (audit AUD-04).
_DEESS_GATE_LO = 1e-4    # -80 dBFS


_DEESS_GATE_HI = 10.0 ** (-70.0 / 20.0)  # -70 dBFS


def _apply_dynamic_deesser(audio: np.ndarray, sr: int,
                           frequency_hz: float = 6500.0,
                           rel_threshold_db: float = -12.0,
                           ratio: float = 6.0,
                           max_reduction_db: float = 12.0) -> np.ndarray:
    from scipy.signal import butter, sosfiltfilt, lfilter

    nyq = sr * 0.5
    lo = max(2000.0, frequency_hz / 1.6)
    hi = min(nyq * 0.95, frequency_hz * 1.6)
    if lo >= hi:
        return audio  # sample rate too low for the requested band

    sos = butter(4, [lo / nyq, hi / nyq], btype="band", output="sos")

    was_1d = audio.ndim == 1
    x = audio[np.newaxis, :] if was_1d else audio
    out = np.empty_like(x)

    # Same vectorised one-pole follower as the static de-esser (~8 ms).
    a = float(np.exp(-1.0 / (0.008 * sr)))
    thresh_lin = 10.0 ** (rel_threshold_db / 20.0)
    inv_ratio = 1.0 / max(1.0, ratio)
    floor = 10.0 ** (-max_reduction_db / 20.0)

    for c in range(x.shape[0]):
        band = sosfiltfilt(sos, x[c])
        band_env = lfilter([1.0 - a], [1.0, -a], np.abs(band))
        full_env = lfilter([1.0 - a], [1.0, -a], np.abs(x[c]))

        # Program-relative detector, gated on actual signal presence: during
        # silence/fade-in (full_env ≈ 0) the filtfilt edge transient in
        # band_env would make the ratio explode and spuriously trigger, so the
        # detector is faded out at the bottom of the range.
        #
        # The gate ramps rather than switching (audit AUD-04). A hard
        # `full_env > 1e-4` step meant a tail dithering either side of -80 dBFS
        # snapped the detector between 0 and its full value sample-to-sample,
        # modulating band gain on exactly the quiet decays where it is most
        # audible. Ramping across -80..-70 dBFS keeps both end states identical
        # to before -- silent below, untouched above -- and makes the crossing
        # continuous. A ramp rather than a Schmitt trigger because these are
        # already-smoothed envelopes, and it stays vectorised (hysteresis is
        # inherently sequential and would cost a per-sample Python loop).
        rel_raw = band_env / np.maximum(full_env, 1e-9)
        gate = np.clip((full_env - _DEESS_GATE_LO) / (_DEESS_GATE_HI - _DEESS_GATE_LO), 0.0, 1.0)
        rel = gate * rel_raw
        over = np.maximum(rel / thresh_lin, 1e-12)
        gr = np.where(over > 1.0, over ** (inv_ratio - 1.0), 1.0)
        gr = np.maximum(gr, floor)

        out[c] = (x[c] - band) + band * gr

    return out[0] if was_1d else out


def _ar_follower(rect: np.ndarray, atk: float, rel: float) -> np.ndarray:
    """Asymmetric attack/release one-pole envelope follower.

    ``e = c*e + (1-c)*x`` with ``c = atk`` while the input rises above the
    envelope and ``rel`` otherwise. The branch makes it non-linear, so it has
    no exact lfilter form; iterating plain Python floats (instead of indexing
    numpy scalars) gives bit-identical results several times faster.
    """
    out = [0.0] * rect.shape[0]
    e = 0.0
    one_minus_atk = 1.0 - atk
    one_minus_rel = 1.0 - rel
    for i, level in enumerate(rect.tolist()):
        if level > e:
            e = atk * e + one_minus_atk * level
        else:
            e = rel * e + one_minus_rel * level
        out[i] = e
    return np.asarray(out, dtype=np.float64)


def _smooth_toward(target: np.ndarray, down_coeff: float, up_coeff: float,
                   start: float = 1.0) -> np.ndarray:
    """One-pole gain smoother: ``down_coeff`` while the target is below the
    current value (gain falling = attack), ``up_coeff`` otherwise. Plain-float
    loop, same result as the per-sample numpy loop it replaces (§6.3)."""
    out = [0.0] * target.shape[0]
    cur = float(start)
    for i, desired in enumerate(target.tolist()):
        c = down_coeff if desired < cur else up_coeff
        cur = c * cur + (1.0 - c) * desired
        out[i] = cur
    return np.asarray(out, dtype=np.float64)


def _linked_detector(x: np.ndarray, sr: int, rms: bool = False) -> np.ndarray:
    """Channel-LINKED detector level, one value per sample (audit CPL-04).

    The max over channels of each channel's own level - peak ``|x_c|`` or a
    ~5 ms RMS of ``x_c``. The old detector rectified the signed channel MEAN,
    so anti-phase stereo 19 dB over threshold produced 0 dB of gain reduction,
    left-only content was under-detected by 6 dB and a 5.1 centre-only signal
    by ~14 dB. Taking the loudest channel detects every one of those at its
    true level while one shared gain keeps the image from shifting.
    """
    if x.ndim == 1:
        x = x[np.newaxis, :]
    if rms:
        from scipy.signal import lfilter
        rms_coeff = float(np.exp(-1.0 / (0.005 * sr)))
        env_sq = lfilter([1.0 - rms_coeff], [1.0, -rms_coeff], x * x, axis=-1)
        return np.sqrt(np.maximum(env_sq, 0.0).max(axis=0))
    return np.abs(x).max(axis=0)


def _soft_knee_gr_db(level_db: np.ndarray, inv_ratio: float, knee_db: float) -> np.ndarray:
    """Giannoulis soft-knee gain computer, vectorised (dB of gain, <= 0):
    0 below the knee, full ratio above it, a quadratic through it."""
    half = max(0.0, knee_db) * 0.5
    slope = inv_ratio - 1.0
    gr = np.zeros_like(level_db)
    above = level_db >= half
    gr[above] = level_db[above] * slope
    if half > 0.0:
        knee = (~above) & (level_db > -half)
        gr[knee] = ((level_db[knee] + half) ** 2 / (2.0 * max(knee_db, 1e-6))) * slope
    return gr


def _auto_release_envelope(rect: np.ndarray, atk: float, rel_fast: float, rel_slow: float,
                           thresh_lin: float, inv_ratio: float, knee_db: float) -> np.ndarray:
    """Program-dependent-release follower for the glue compressor: the release
    coefficient slides from ``rel_fast`` to ``rel_slow`` with the previous
    sample's gain-reduction depth (0..18 dB). Plain-float loop; the gain
    computer itself runs vectorised afterwards on the returned envelope."""
    import math

    half = max(0.0, knee_db) * 0.5
    kd = 2.0 * max(knee_db, 1e-6)
    slope = inv_ratio - 1.0
    log_thr = math.log10(max(thresh_lin, 1e-12))
    out = [0.0] * rect.shape[0]
    e = 0.0
    prev_gr_db = 0.0
    span = rel_slow - rel_fast
    for i, level in enumerate(rect.tolist()):
        if level > e:
            c = atk
        else:
            depth = prev_gr_db / 18.0
            c = rel_fast + span * (depth if depth < 1.0 else 1.0)
        e = c * e + (1.0 - c) * level
        out[i] = e
        ld = 20.0 * (math.log10(e if e > 1e-12 else 1e-12) - log_thr)
        if ld <= -half:
            prev_gr_db = 0.0
        elif ld >= half:
            prev_gr_db = -ld * slope
        else:
            prev_gr_db = -((ld + half) ** 2 / kd) * slope
    return np.asarray(out, dtype=np.float64)


# ---------------------------------------------------------------------------
# Sidechain-HPF compressor — a real compressor whose detector is high-passed
# so low-frequency energy doesn't over-trigger gain reduction on broadband
# sources (drum overheads, vocals, full mixes). Optionally keyed from an
# external sidechain signal instead of the input itself.
# ---------------------------------------------------------------------------
def _apply_sidechain_compressor(audio: np.ndarray, sr: int,
                                threshold_db: float = -18.0,
                                ratio: float = 4.0,
                                attack_ms: float = 10.0,
                                release_ms: float = 120.0,
                                sidechain_hpf_hz: float = 0.0,
                                makeup_db: float = 0.0,
                                mix: float = 1.0,
                                key: np.ndarray | None = None) -> np.ndarray:
    from scipy.signal import butter, sosfilt

    was_1d = audio.ndim == 1
    x = audio[np.newaxis, :] if was_1d else audio

    # Detector signal: the external key or the input, high-passed per channel
    # and then linked on the loudest channel (CPL-04).
    det_src = key if key is not None else x
    if det_src.ndim == 1:
        det_src = det_src[np.newaxis, :]
    det_src = np.asarray(det_src, dtype=np.float64)

    if sidechain_hpf_hz and sidechain_hpf_hz > 20.0:
        sos = butter(2, min(sidechain_hpf_hz, sr * 0.45) / (sr * 0.5),
                     btype="high", output="sos")
        det_src = sosfilt(sos, det_src, axis=-1)

    atk = float(np.exp(-1.0 / (max(0.1, attack_ms) * 0.001 * sr)))
    rel = float(np.exp(-1.0 / (max(1.0, release_ms) * 0.001 * sr)))
    rect = _linked_detector(det_src, sr)

    # Signal-dependent attack/release follower.
    env = _ar_follower(rect, atk, rel)

    thresh_lin = 10.0 ** (threshold_db / 20.0)
    inv_ratio = 1.0 / max(1.0, ratio)
    over = np.maximum(env / thresh_lin, 1e-12)   # floor avoids 0**neg warnings
    gain = np.where(over > 1.0, over ** (inv_ratio - 1.0), 1.0)
    gain *= 10.0 ** (makeup_db / 20.0)

    wet = x * gain[np.newaxis, :]
    mix = float(np.clip(mix, 0.0, 1.0))
    out = x * (1.0 - mix) + wet * mix
    return out[0] if was_1d else out


# ---------------------------------------------------------------------------
# Glue compressor — soft-knee bus compressor with a peak/RMS detector choice
# and an optional program-dependent ("auto") release, aimed at SSL/API-style
# bus glue rather than the hard-knee, fixed-release pedalboard Compressor.
# Program-dependent release: the release time constant lengthens with the
# depth of gain reduction (a bigger squeeze relaxes more slowly), which is
# what gives an "auto" bus compressor its glue character instead of pumping.
# The detector is channel-linked on the loudest channel (CPL-04); only the
# envelope follower is sequential, the gain computer is vectorised (§6.3).
# ---------------------------------------------------------------------------
def _apply_glue_compressor(audio: np.ndarray, sr: int,
                           threshold_db: float = -18.0,
                           ratio: float = 2.0,
                           attack_ms: float = 10.0,
                           release_ms: float = 100.0,   # ignored when auto_release=1
                           knee_db: float = 6.0,
                           makeup_db: float = 0.0,
                           detector: float = 0.0,        # 0 = peak, 1 = RMS
                           auto_release: float = 0.0,
                           mix: float = 1.0) -> np.ndarray:
    was_1d = audio.ndim == 1
    x = audio[np.newaxis, :] if was_1d else audio

    # Peak: max |x_c|. RMS (~5 ms one-pole on x_c^2, per channel, then linked)
    # responds to average energy rather than instantaneous peaks, giving the
    # smoother, less pumping character RMS-detecting bus comps have.
    rect = _linked_detector(x, sr, rms=detector >= 0.5)

    atk = float(np.exp(-1.0 / (max(0.1, attack_ms) * 0.001 * sr)))
    rel_fast = float(np.exp(-1.0 / (max(1.0, release_ms) * 0.001 * sr)))
    rel_slow = float(np.exp(-1.0 / (max(1.0, release_ms) * 4.0 * 0.001 * sr)))
    thresh_lin = 10.0 ** (threshold_db / 20.0)
    inv_ratio = 1.0 / max(1.0, ratio)

    if auto_release >= 0.5:
        env = _auto_release_envelope(rect, atk, rel_fast, rel_slow, thresh_lin, inv_ratio, knee_db)
    else:
        env = _ar_follower(rect, atk, rel_fast)

    level_db = 20.0 * np.log10(np.maximum(env, 1e-12) / max(thresh_lin, 1e-12))
    gr_db_trace = _soft_knee_gr_db(level_db, inv_ratio, knee_db)

    gain = (10.0 ** (gr_db_trace / 20.0)) * (10.0 ** (makeup_db / 20.0))
    wet = x * gain[np.newaxis, :]
    mix = float(np.clip(mix, 0.0, 1.0))
    out = x * (1.0 - mix) + wet * mix
    return out[0] if was_1d else out


# ---------------------------------------------------------------------------
# LeakGate — inter-stem bleed cleanup for Demucs/htdemucs stems. A multiband
# downward expander removes low-level residual bleed (which sits *between* the
# wanted content) without touching the music, plus an air-band cut for the
# 4 kHz+ RVQ "ghosting" that separation leaves on AI stems. The per-frame ML
# bleed classifier (the trained model) slots in later where the heuristic
# envelope gate is now; presets shape the aggressiveness (2026-07-15 plan §C).
# ---------------------------------------------------------------------------
_LEAK_GATE_PRESETS: dict[str, dict[str, float]] = {
    # gain: overall trim; gr_db: max per-band gate reduction; thr_db: gate
    # threshold rel. to band peak; mid_protect: reduce GR in 300-3k; air_cut_db:
    # extra cut above 8 kHz.
    "default":    {"gain": 0.94, "gr_db": 12.0, "thr_db": -40.0, "mid_protect": 0.5, "air_cut_db": 0.0},
    "vocal":      {"gain": 0.94, "gr_db": 14.0, "thr_db": -38.0, "mid_protect": 0.7, "air_cut_db": -1.5},
    "instrument": {"gain": 0.96, "gr_db": 9.0,  "thr_db": -44.0, "mid_protect": 0.4, "air_cut_db": 0.0},
    "master":     {"gain": 0.98, "gr_db": 4.0,  "thr_db": -50.0, "mid_protect": 0.6, "air_cut_db": 0.0},
    "ai_ghost":   {"gain": 0.95, "gr_db": 12.0, "thr_db": -40.0, "mid_protect": 0.5, "air_cut_db": -4.0},
    "ai_heavy":   {"gain": 0.92, "gr_db": 18.0, "thr_db": -36.0, "mid_protect": 0.4, "air_cut_db": -3.0},
    "aggressive": {"gain": 0.90, "gr_db": 24.0, "thr_db": -32.0, "mid_protect": 0.2, "air_cut_db": -2.0},
}


def _frame_probs_to_samples(probs: np.ndarray, hop: int, n: int, sr: int,
                            smooth_ms: float = 5.0) -> np.ndarray:
    """Per-frame bleed probabilities -> a smooth per-sample curve (audit CPL-21).

    The classifier's STFT is centred (librosa ``center=True``), so frame ``i``
    describes sample ``i * hop``; ``np.repeat`` both stepped the gain 12-24 dB
    at every 512-sample boundary (zipper noise) and placed each step half a
    frame late. Linear interpolation between frame centres removes the steps
    and the offset, and a short zero-phase one-pole pass rounds the corners.
    """
    from scipy.signal import filtfilt

    probs = np.asarray(probs, dtype=np.float64)
    if probs.size == 0 or n <= 0:
        return np.zeros(max(0, n))
    centres = np.arange(probs.shape[0], dtype=np.float64) * float(hop)
    curve = np.interp(np.arange(n, dtype=np.float64), centres, probs)
    a = float(np.exp(-1.0 / max(1.0, smooth_ms * 1e-3 * sr)))
    if n > 12:
        curve = filtfilt([1.0 - a], [1.0, -a], curve, padlen=min(n - 1, 3 * int(1.0 / (1.0 - a) + 1)))
    return np.clip(curve, 0.0, 1.0)


def _apply_leak_gate(audio: np.ndarray, sr: int, preset: str = "default") -> np.ndarray:
    p = _LEAK_GATE_PRESETS.get(preset, _LEAK_GATE_PRESETS["default"])
    was_1d = audio.ndim == 1
    x = audio[np.newaxis, :] if was_1d else audio

    edges = [0.0, 120.0, 300.0, 3000.0, 8000.0, sr]   # sub / low / mid / high / air
    bands = [_band_split(x, sr, lo_hz=edges[i], hi_hz=edges[i + 1]) for i in range(len(edges) - 1)]

    # Optional learned mode: a per-band, per-frame bleed probability drives the
    # reduction instead of the fixed envelope threshold. None (the default —
    # no model / opt-in off) keeps the deterministic envelope behavior.
    ml_probs = None
    lk_hop = 512
    try:
        from khensu_services.copilot.leak_gate_classifier import LeakGateInference, _HOP
        lk_hop = _HOP
        ml_probs = LeakGateInference().predict(x.mean(axis=0), sr)   # (N_BANDS, frames)
    except Exception:
        ml_probs = None

    win = max(1, int(0.02 * sr))                       # 20 ms envelope
    out = np.zeros_like(x)
    for bi, band in enumerate(bands):
        is_mid = edges[bi] >= 300.0 and edges[bi + 1] <= 3000.0
        is_air = edges[bi] >= 8000.0
        gr = p["gr_db"] * (1.0 - p["mid_protect"]) if is_mid else p["gr_db"]

        band_bleed = None
        if ml_probs is not None and bi < ml_probs.shape[0]:
            band_bleed = _frame_probs_to_samples(ml_probs[bi], lk_hop, band.shape[1], sr)

        for c in range(band.shape[0]):
            env = np.sqrt(np.convolve(band[c] ** 2, np.ones(win) / win, mode="same") + 1e-12)
            peak = float(np.max(env)) + 1e-12
            env_db = 20.0 * np.log10(env / peak)
            if band_bleed is not None:
                # Learned reduction: scale by the predicted per-band bleed prob.
                below = np.clip(band_bleed, 0.0, 1.0)
            else:
                # Downward expander: full reduction ~20 dB below the threshold,
                # ramping from 0 at the threshold (a steeper, gate-like curve).
                below = np.clip((p["thr_db"] - env_db) / 20.0, 0.0, 1.0)
            gain_db = -gr * below
            g = 10.0 ** (gain_db / 20.0)
            b = band[c] * g
            if is_air and p["air_cut_db"] < 0.0:
                b = b * (10.0 ** (p["air_cut_db"] / 20.0))
            out[c] += b

    out = out * p["gain"]
    return out[0] if was_1d else out


# ---------------------------------------------------------------------------
# Transient shaper — dual-envelope design (classic "SPL Transient Designer"
# approach): a fast follower tracks transient content, a slow follower tracks
# the sustained/average level, and the gap between them (normalized by the
# slow envelope) drives a continuous blend between attack_gain and
# sustain_gain. No fixed threshold — it responds to the *shape* of the
# signal, not its absolute level.
# ---------------------------------------------------------------------------
def _apply_transient_shaper(audio: np.ndarray, sr: int,
                            attack_gain_db: float = 0.0,
                            sustain_gain_db: float = 0.0,
                            attack_ms: float = 3.0,
                            release_ms: float = 200.0) -> np.ndarray:
    was_1d = audio.ndim == 1
    x = audio[np.newaxis, :] if was_1d else audio
    detector = np.abs(x).mean(axis=0)

    fast_atk = float(np.exp(-1.0 / (0.001 * sr)))   # ~1 ms: catches the transient itself
    fast_rel = float(np.exp(-1.0 / (0.010 * sr)))   # ~10 ms: decays before the next hit
    slow_atk = float(np.exp(-1.0 / (max(1.0, attack_ms) * 0.001 * sr)))
    slow_rel = float(np.exp(-1.0 / (max(1.0, release_ms) * 0.001 * sr)))

    fast_env = _ar_follower(detector, fast_atk, fast_rel)
    slow_env = _ar_follower(detector, slow_atk, slow_rel)

    # transient_amount in [0, 1]: 0 during sustained/steady material, rising
    # toward 1 the more the fast envelope outruns the slow one (a hit).
    diff = fast_env - slow_env
    transient_amount = np.clip(diff / np.maximum(slow_env, 1e-6), 0.0, 1.0)

    attack_gain_lin = 10.0 ** (attack_gain_db / 20.0)
    sustain_gain_lin = 10.0 ** (sustain_gain_db / 20.0)
    gain = sustain_gain_lin + (attack_gain_lin - sustain_gain_lin) * transient_amount

    out = x * gain[np.newaxis, :]
    return out[0] if was_1d else out


# ---------------------------------------------------------------------------
# Multiband compressor — 3 bands (low/mid/high) via the zero-phase Butterworth
# splits _band_split already uses elsewhere, each fed through the same
# soft-knee compressor core as GlueCompressor. Independent threshold/ratio
# per band is the one thing a single broadband compressor structurally can't
# do: tame a boomy low end without pumping the vocal, or vice versa.
# ---------------------------------------------------------------------------
def _apply_multiband_compressor(audio: np.ndarray, sr: int,
                                low_crossover_hz: float = 200.0,
                                high_crossover_hz: float = 3000.0,
                                low_threshold_db: float = -24.0, low_ratio: float = 3.0,
                                mid_threshold_db: float = -20.0, mid_ratio: float = 2.5,
                                high_threshold_db: float = -18.0, high_ratio: float = 2.0,
                                attack_ms: float = 10.0, release_ms: float = 120.0,
                                makeup_db: float = 0.0) -> np.ndarray:
    was_1d = audio.ndim == 1
    x = audio[np.newaxis, :] if was_1d else audio

    lo = _band_split(x, sr, lo_hz=0.0, hi_hz=low_crossover_hz)
    mid = _band_split(x, sr, lo_hz=low_crossover_hz, hi_hz=high_crossover_hz)
    hi = _band_split(x, sr, lo_hz=high_crossover_hz, hi_hz=sr)   # hi_hz >= nyq*0.95 skips the lowpass

    lo_c = _apply_glue_compressor(lo, sr, threshold_db=low_threshold_db, ratio=low_ratio,
                                  attack_ms=attack_ms, release_ms=release_ms, knee_db=6.0)
    mid_c = _apply_glue_compressor(mid, sr, threshold_db=mid_threshold_db, ratio=mid_ratio,
                                   attack_ms=attack_ms, release_ms=release_ms, knee_db=6.0)
    hi_c = _apply_glue_compressor(hi, sr, threshold_db=high_threshold_db, ratio=high_ratio,
                                  attack_ms=attack_ms, release_ms=release_ms, knee_db=6.0)

    summed = lo_c + mid_c + hi_c
    out = summed * (10.0 ** (makeup_db / 20.0))
    return out[0] if was_1d else out


# ---------------------------------------------------------------------------
# Dynamic EQ — a single threshold-triggered EQ band: the band is cut (per the
# ratio) only while its own level exceeds the threshold, and is left completely
# untouched otherwise. This is the surgical tool the Dashboard's "Dynamic EQ
# Suggestions" panel recommends for intermittent resonances: a static PeakFilter
# cut dulls the whole track, a broadband compressor can't isolate the frequency.
# One band per instance — chain several DynamicEQ stages for multiple bands
# (the flat per-effect parameter schema has no variable-length band list).
# Same isolate/duck/recombine structure as _apply_deesser, generalised to any
# centre frequency and Q, with a real attack/release detector and a
# max_reduction_db safety floor so extreme settings stay surgical.
# ---------------------------------------------------------------------------
def _apply_dynamic_eq(audio: np.ndarray, sr: int,
                      frequency_hz: float = 3000.0,
                      q: float = 2.0,
                      threshold_db: float = -30.0,
                      ratio: float = 4.0,
                      attack_ms: float = 5.0,
                      release_ms: float = 120.0,
                      max_reduction_db: float = 24.0) -> np.ndarray:
    from scipy.signal import butter, sosfiltfilt

    nyq = sr * 0.5
    # Octave-symmetric band edges: bandwidth ≈ 1/q octaves around the centre.
    k = 2.0 ** (1.0 / (2.0 * max(0.3, q)))
    lo = frequency_hz / k
    hi = min(nyq * 0.95, frequency_hz * k)
    if lo >= hi or lo < 10.0:
        return audio   # sample rate too low for the requested band

    sos = butter(4, [lo / nyq, hi / nyq], btype="band", output="sos")

    was_1d = audio.ndim == 1
    x = audio[np.newaxis, :] if was_1d else audio

    # Zero-phase band isolation per channel, so subtract-and-recombine is
    # comb-free (same rationale as the de-esser).
    bands = np.stack([sosfiltfilt(sos, x[c]) for c in range(x.shape[0])])

    # Linked-stereo detector: one envelope from the mean band level, applied to
    # every channel, so gain riding never shifts the stereo image.
    rect = np.abs(bands).mean(axis=0)
    atk = float(np.exp(-1.0 / (max(0.1, attack_ms) * 0.001 * sr)))
    rel = float(np.exp(-1.0 / (max(1.0, release_ms) * 0.001 * sr)))
    env = _ar_follower(rect, atk, rel)

    thresh_lin = 10.0 ** (threshold_db / 20.0)
    inv_ratio = 1.0 / max(1.0, ratio)
    over = np.maximum(env / thresh_lin, 1e-12)
    gr = np.where(over > 1.0, over ** (inv_ratio - 1.0), 1.0)
    gr = np.maximum(gr, 10.0 ** (-max_reduction_db / 20.0))

    out = (x - bands) + bands * gr[np.newaxis, :]
    return out[0] if was_1d else out


# ---------------------------------------------------------------------------
# Clipper — pre-limiter peak control, distinct from Limiter (gain-reduction
# envelope) and TapeSaturation (broadband harmonic colour). A clipper simply
# removes anything above the ceiling; `hardness` blends from a smooth tanh
# soft-clip (gentler harmonics) to a hard brickwall. 4x oversampled so the
# harmonics a hard clip generates above Nyquist alias back cleanly instead of
# folding into the audible band (same rationale as _apply_tape_saturation).
# ---------------------------------------------------------------------------
def _apply_clipper(audio: np.ndarray, sr: int,
                   ceiling_db: float = -0.3,
                   hardness: float = 0.5,
                   mix: float = 1.0) -> np.ndarray:
    from scipy.signal import resample_poly

    was_1d = audio.ndim == 1
    x = audio[np.newaxis, :] if was_1d else audio
    ceiling = 10.0 ** (ceiling_db / 20.0)
    hardness = float(np.clip(hardness, 0.0, 1.0))
    mix = float(np.clip(mix, 0.0, 1.0))

    out = np.empty_like(x)
    for c in range(x.shape[0]):
        up = resample_poly(x[c], 4, 1)
        soft = ceiling * np.tanh(up / max(ceiling, 1e-9))
        hard = np.clip(up, -ceiling, ceiling)
        clipped = soft * (1.0 - hardness) + hard * hardness
        down = resample_poly(clipped, 1, 4)
        n = min(down.shape[0], x.shape[1])
        out[c, :n] = down[:n]
        if n < x.shape[1]:
            out[c, n:] = x[c, n:]

    # The decimation filter's ringing at the clip discontinuities can
    # overshoot past `ceiling` even though the oversampled signal never did.
    # A base-rate hard clamp here (the old fix) re-introduced exactly the
    # aliased hard-clip products the 4x path exists to avoid, and only held a
    # sample peak; the true-peak limiter catches the small overshoot smoothly
    # and holds the ceiling between samples too (AUDIT-082).
    wet = true_peak_limit(out, sr, ceiling_dbtp=ceiling_db)
    result = x * (1.0 - mix) + wet * mix
    return result[0] if was_1d else result


def _apply_limiter(audio: np.ndarray, sr: int, threshold_db: float = -1.0,
                   release_ms: float = 60.0) -> np.ndarray:
    """The chain's "Limiter": a true-peak ceiling at ``threshold_db`` dBTP
    (lookahead, linked, no makeup gain). Loudness comes from an explicit Gain
    stage in front of it, never from the limiter itself."""
    return true_peak_limit(audio, sr, ceiling_dbtp=threshold_db, release_ms=release_ms)


# ---------------------------------------------------------------------------
# Plosive remover — low-band onset attenuation.  It isolates the rumble/body
# band, watches for a fast rise over its recent baseline, and ducks only that
# band.  Sustained low notes/vowels do not remain reduced after the onset.
# ---------------------------------------------------------------------------
def _apply_plosive_remover(audio: np.ndarray, sr: int,
                           frequency_hz: float = 180.0,
                           threshold_db: float = -36.0,
                           reduction_db: float = 10.0,
                           attack_ms: float = 1.0,
                           release_ms: float = 80.0,
                           mix: float = 1.0) -> np.ndarray:
    from scipy.signal import butter, lfilter, sosfiltfilt

    nyq = sr * 0.5
    cutoff = min(float(frequency_hz), nyq * 0.9)
    if cutoff <= 30.0:
        return audio
    sos = butter(4, cutoff / nyq, btype="low", output="sos")
    was_1d = audio.ndim == 1
    x = audio[np.newaxis, :] if was_1d else audio
    out = np.empty_like(x)
    threshold = 10.0 ** (threshold_db / 20.0)
    floor = 10.0 ** (-reduction_db / 20.0)
    fast_coeff = float(np.exp(-1.0 / (0.003 * sr)))
    baseline_coeff = float(np.exp(-1.0 / (0.150 * sr)))
    attack_coeff = float(np.exp(-1.0 / (max(0.1, attack_ms) * 0.001 * sr)))
    release_coeff = float(np.exp(-1.0 / (max(1.0, release_ms) * 0.001 * sr)))

    for c in range(x.shape[0]):
        band = sosfiltfilt(sos, x[c])
        fast = lfilter([1.0 - fast_coeff], [1.0, -fast_coeff], np.abs(band))
        baseline = lfilter([1.0 - baseline_coeff], [1.0, -baseline_coeff], np.abs(band))
        # A 4 dB rise over the 150 ms baseline distinguishes a consonant burst
        # from sustained chest/body energy.  The absolute threshold keeps
        # silence and filter transients from triggering it.
        onset = (fast >= threshold) & (fast >= baseline * (10.0 ** (4.0 / 20.0)))
        target = np.where(onset, floor, 1.0)
        gain = _smooth_toward(target, attack_coeff, release_coeff)
        wet = (x[c] - band) + band * gain
        out[c] = x[c] * (1.0 - mix) + wet * mix

    return out[0] if was_1d else out


# ---------------------------------------------------------------------------
# SpectralDenoise (FIX-B) - steady broadband hiss on a bounced file. The noise
# profile is each FFT bin's low percentile over the track (in a mix, a bin's
# quietest frames are its noise), and each frame keeps
# sqrt(max(1 - sensitivity * N / P, floor)) of its magnitude - a power-domain
# Wiener-style gain with a floor of -reduction_db, smoothed over neighbouring
# bins and released slowly over time so it does not "chirp". Frames are
# processed in batches (bounded memory on whole tracks, PERF-7).
# ---------------------------------------------------------------------------
_DENOISE_N_FFT = 2048
_DENOISE_HOP = 512
_DENOISE_PROFILE_PERCENTILE = 10.0
_DENOISE_RELEASE = 0.7  # per-hop smoothing when the gain falls


def _apply_spectral_denoise(audio: np.ndarray, sr: int, reduction_db: float = 12.0,
                            sensitivity: float = 3.0) -> np.ndarray:
    from numpy.lib.stride_tricks import sliding_window_view

    was_1d = audio.ndim == 1
    x = audio[np.newaxis, :] if was_1d else audio
    n_fft, hop = _DENOISE_N_FFT, _DENOISE_HOP
    floor_pow = 10.0 ** (-abs(float(reduction_db)) / 10.0)
    window = np.hanning(n_fft + 1)[:-1]  # periodic: constant overlap-add at hop n/4
    out = np.empty_like(x, dtype=np.float64)
    for c in range(x.shape[0]):
        sig = np.asarray(x[c], dtype=np.float64)
        n = sig.shape[0]
        if n < n_fft:
            out[c] = sig
            continue
        padded = np.pad(sig, (n_fft, n_fft + hop))
        frames = sliding_window_view(padded, n_fft)[::hop]
        # Noise profile from every 4th frame: the percentile needs the spread,
        # not every frame, and this keeps the profile pass at a quarter.
        sample = np.abs(np.fft.rfft(frames[::4] * window, axis=1)) ** 2
        # A noise bin's power over frames is exponential (chi-squared, two
        # degrees of freedom), so its p-th percentile is -ln(1 - p) of its
        # mean: rescale the low percentile to the mean noise power.
        noise = np.percentile(sample, _DENOISE_PROFILE_PERCENTILE, axis=0) / -np.log1p(
            -_DENOISE_PROFILE_PERCENTILE / 100.0)
        del sample
        acc = np.zeros(padded.shape[0])
        norm = np.zeros(padded.shape[0])
        prev = np.ones(n_fft // 2 + 1)
        kernel = np.array([0.25, 0.5, 0.25])
        for f0 in range(0, frames.shape[0], 256):
            spec = np.fft.rfft(frames[f0:f0 + 256] * window, axis=1)
            power = spec.real ** 2 + spec.imag ** 2
            gain = np.maximum(1.0 - sensitivity * noise / np.maximum(power, 1e-30), floor_pow)
            for i in range(gain.shape[0]):
                g = np.convolve(gain[i], kernel, mode="same")
                g = np.where(g < prev, _DENOISE_RELEASE * prev + (1.0 - _DENOISE_RELEASE) * g, g)
                gain[i] = g
                prev = g
            y = np.fft.irfft(spec * np.sqrt(gain), n=n_fft, axis=1) * window
            for i in range(y.shape[0]):
                s0 = (f0 + i) * hop
                acc[s0:s0 + n_fft] += y[i]
                norm[s0:s0 + n_fft] += window ** 2
        out[c] = (acc / np.maximum(norm, 1e-9))[n_fft:n_fft + n]
    return out[0] if was_1d else out
