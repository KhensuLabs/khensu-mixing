"""Fitted processing chains (CE-AI-3).

Fits a *readable* chain - a low shelf, a high shelf, up to four peaking bands
(frequency / gain / Q each) and one compressor (threshold / ratio / attack /
release) plus a makeup gain - that moves a mix's band spectrum and its crest
factor / loudness range toward a reference file or a genre target. Every
parameter is a named knob the user can read and change; nothing is a black box.

How:

* **Spectrum.** Both files are reduced to ~1/3-octave band levels (mean power
  per bin, from ``dsp_metrics.averaged_power_spectrum`` + ``band_bin_index``).
  The target is the level-free band difference (reference - mix, mean
  removed), scaled by ``amount`` (FEAT-3). The EQ response at the band
  centres is computed analytically from the same biquad formulas the renderer
  (pedalboard / JUCE ``IIRCoefficients``) uses, and ``scipy.optimize``
  (L-BFGS-B, bounded) fits the shelves, then adds peaks greedily at the
  largest remaining error while each one still pays for itself.
* **Dynamics.** The compressor is fitted on a cheap frame model (20 ms frames:
  peak envelope with attack / release ballistics, static gain computer,
  partial transient escape during the attack) that predicts crest and LRA
  changes; ``scipy.optimize.differential_evolution`` (seeded, so the result
  is deterministic) searches threshold / ratio / attack / release. A
  compressor can only *reduce* crest and LRA, so when the target is more
  dynamic than the mix none is fitted and the notes say why.
* **Level.** A makeup gain keeps integrated loudness about where it was; the
  renderer's clip guard keeps true peak under -1 dBTP.

The chain is emitted in the ``/copilot/apply_chain`` effect format
(``LowShelfFilter``, ``PeakFilter``, ``HighShelfFilter``, ``Compressor``,
``Gain`` - the names ``copilot/dsp.py`` renders), so rendering it runs the
shared validate -> quarantine -> render -> guardrail pipeline. The response
carries the error before, the error the model predicts, and the error
measured on the rendered file.

Genre targets: ``resources/genre_norms`` carries no band spectrum, only a
spectral centroid and an LRA range, so a genre target is a spectral *tilt*
that moves the mix's centroid to the genre's, plus the LRA range. The notes
say so.
"""
from __future__ import annotations

import math
from typing import Any, Callable

import numpy as np
import soundfile as sf

N_FFT = 8192
BAND_LO_HZ = 31.5
BAND_HI_HZ = 16000.0
BANDS_PER_OCTAVE = 3
MAX_PEAKS = 4
MAX_GAIN_DB = 9.0
SHELF_Q = 0.707
FRAME_SEC = 0.02
MAX_FRAMES = 9000
# A peak must lower the weighted RMS band error by at least this much to stay.
PEAK_MIN_IMPROVEMENT_DB = 0.05
PEAK_MIN_GAIN_DB = 0.5
SHELF_MIN_GAIN_DB = 0.3
_REG = 0.002          # gain^2 penalty: prefer the smaller of two equal fits


# --- band analysis -----------------------------------------------------------

def band_edges() -> np.ndarray:
    n = int(round(math.log2(BAND_HI_HZ / BAND_LO_HZ) * BANDS_PER_OCTAVE))
    return BAND_LO_HZ * 2.0 ** (np.arange(n + 1) / BANDS_PER_OCTAVE)


def _mono(y: np.ndarray) -> np.ndarray:
    return y.mean(axis=1) if y.ndim == 2 else y


def band_levels_db(y: np.ndarray, sr: int, edges: np.ndarray | None = None
                   ) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """``(centers_hz, level_db, power)`` per band: mean power per FFT bin in
    each band of the mono mix (shared ``dsp_metrics`` STFT helpers)."""
    from khensu_services.analysis.dsp_metrics import (
        averaged_power_spectrum, band_bin_index, merge_log_band_edges)
    e = band_edges() if edges is None else edges
    e = e[e < sr / 2.0]
    freqs = np.fft.rfftfreq(N_FFT, d=1.0 / sr)
    e = merge_log_band_edges(e, freqs)
    spec = averaged_power_spectrum(_mono(y), N_FFT)
    band_of_bin, valid = band_bin_index(freqs, e)
    n_bands = len(e) - 1
    sums = np.bincount(band_of_bin[valid], weights=spec[valid], minlength=n_bands)[:n_bands]
    counts = np.bincount(band_of_bin[valid], minlength=n_bands)[:n_bands]
    power = sums / np.maximum(counts, 1)
    centers = np.sqrt(e[:-1] * e[1:])
    return centers, 10.0 * np.log10(power + 1e-20), power


def band_weights(centers: np.ndarray) -> np.ndarray:
    """The extreme bands matter less (and are measured less reliably)."""
    w = np.ones_like(centers)
    w[(centers < 40.0) | (centers > 14000.0)] = 0.5
    return w


def _wmean(x: np.ndarray, w: np.ndarray) -> float:
    return float(np.sum(x * w) / np.sum(w))


def _wrms(x: np.ndarray, w: np.ndarray) -> float:
    return float(np.sqrt(np.sum(w * x * x) / np.sum(w)))


def level_free(delta: np.ndarray, w: np.ndarray) -> np.ndarray:
    return delta - _wmean(delta, w)


# --- EQ response (JUCE IIRCoefficients / pedalboard formulas) -----------------

def _biquad_db(b: tuple[float, float, float], a: tuple[float, float, float],
               f: np.ndarray, sr: int) -> np.ndarray:
    z1 = np.exp(-1j * 2.0 * np.pi * f / sr)
    z2 = z1 * z1
    h = (b[0] + b[1] * z1 + b[2] * z2) / (a[0] + a[1] * z1 + a[2] * z2)
    return 20.0 * np.log10(np.abs(h) + 1e-20)


def peak_db(f: np.ndarray, sr: int, fc: float, gain_db: float, q: float) -> np.ndarray:
    fc = min(fc, 0.45 * sr)
    A = math.sqrt(10.0 ** (gain_db / 20.0))
    w = 2.0 * math.pi * fc / sr
    alpha = 0.5 * math.sin(w) / q
    c2 = -2.0 * math.cos(w)
    return _biquad_db((1 + alpha * A, c2, 1 - alpha * A), (1 + alpha / A, c2, 1 - alpha / A), f, sr)


def _shelf(f: np.ndarray, sr: int, fc: float, gain_db: float, q: float, high: bool) -> np.ndarray:
    fc = min(fc, 0.45 * sr)
    A = math.sqrt(10.0 ** (gain_db / 20.0))
    am1, ap1 = A - 1.0, A + 1.0
    w = 2.0 * math.pi * fc / sr
    coso = math.cos(w)
    beta = math.sin(w) * math.sqrt(A) / q
    amc = am1 * coso
    if not high:
        b = (A * (ap1 - amc + beta), A * 2.0 * (am1 - ap1 * coso), A * (ap1 - amc - beta))
        a = (ap1 + amc + beta, -2.0 * (am1 + ap1 * coso), ap1 + amc - beta)
    else:
        b = (A * (ap1 + amc + beta), A * -2.0 * (am1 + ap1 * coso), A * (ap1 + amc - beta))
        a = (ap1 - amc + beta, 2.0 * (am1 - ap1 * coso), ap1 - amc - beta)
    return _biquad_db(b, a, f, sr)


def low_shelf_db(f, sr, fc, gain_db, q=SHELF_Q):
    return _shelf(f, sr, fc, gain_db, q, high=False)


def high_shelf_db(f, sr, fc, gain_db, q=SHELF_Q):
    return _shelf(f, sr, fc, gain_db, q, high=True)


# Parameter vector: [log2 ls_f, ls_g, log2 hs_f, hs_g, (log2 f, g, q) * peaks]
_LS_F = (40.0, 400.0)
_HS_F = (2500.0, 14000.0)
_PK_F = (50.0, 12000.0)
_PK_Q = (0.4, 4.0)


def _bounds(n_peaks: int) -> list[tuple[float, float]]:
    b = [(math.log2(_LS_F[0]), math.log2(_LS_F[1])), (-MAX_GAIN_DB, MAX_GAIN_DB),
         (math.log2(_HS_F[0]), math.log2(_HS_F[1])), (-MAX_GAIN_DB, MAX_GAIN_DB)]
    for _ in range(n_peaks):
        b += [(math.log2(_PK_F[0]), math.log2(_PK_F[1])), (-MAX_GAIN_DB, MAX_GAIN_DB), _PK_Q]
    return b


def eq_response_db(x: np.ndarray, f: np.ndarray, sr: int) -> np.ndarray:
    h = low_shelf_db(f, sr, 2.0 ** x[0], x[1]) + high_shelf_db(f, sr, 2.0 ** x[2], x[3])
    for k in range((len(x) - 4) // 3):
        pf, pg, pq = x[4 + 3 * k: 7 + 3 * k]
        h = h + peak_db(f, sr, 2.0 ** pf, pg, pq)
    return h


def _eq_objective(x, f, sr, target, w) -> float:
    r = level_free(eq_response_db(x, f, sr) - target, w)
    gains = np.asarray([x[1], x[3]] + [x[5 + 3 * k] for k in range((len(x) - 4) // 3)])
    return float(np.sum(w * r * r) / np.sum(w) + _REG * np.sum(gains * gains))


def _stacked(x: np.ndarray, min_octaves: float = 0.25) -> bool:
    """True when two peaks sit within `min_octaves` of each other."""
    fs = sorted(float(x[4 + 3 * k]) for k in range((len(x) - 4) // 3))
    return any(b - a < min_octaves for a, b in zip(fs, fs[1:]))


def fit_eq(centers: np.ndarray, target: np.ndarray, sr: int, max_peaks: int = MAX_PEAKS
           ) -> dict[str, Any]:
    """Fit shelves + up to `max_peaks` peaks to the level-free `target`."""
    from scipy.optimize import minimize
    w = band_weights(centers)
    target = level_free(target, w)

    def _solve(x0: np.ndarray) -> np.ndarray:
        res = minimize(_eq_objective, x0, args=(centers, sr, target, w), method="L-BFGS-B",
                       bounds=_bounds((len(x0) - 4) // 3), options={"maxiter": 300})
        return np.asarray(res.x, dtype=float)

    def _err(x: np.ndarray) -> float:
        return _wrms(level_free(eq_response_db(x, centers, sr) - target, w), w)

    lo = target[centers < 250.0]
    hi = target[centers > 4000.0]
    x = _solve(np.array([math.log2(120.0), float(np.clip(lo.mean() if lo.size else 0.0, -6, 6)),
                         math.log2(8000.0), float(np.clip(hi.mean() if hi.size else 0.0, -6, 6))]))
    err = _err(x)
    for _ in range(max(0, min(MAX_PEAKS, max_peaks))):
        resid = level_free(target - eq_response_db(x, centers, sr), w)
        inside = (centers >= _PK_F[0]) & (centers <= _PK_F[1])
        if not inside.any():
            break
        k = int(np.argmax(np.where(inside, np.abs(resid) * w, -1.0)))
        cand = _solve(np.concatenate([x, [math.log2(centers[k]),
                                          float(np.clip(resid[k], -MAX_GAIN_DB, MAX_GAIN_DB)), 1.4]]))
        cand_err = _err(cand)
        if err - cand_err < PEAK_MIN_IMPROVEMENT_DB or _stacked(cand):
            # Not worth a band - or the optimiser parked it on top of an
            # existing one to exceed the gain limit, which reads as nonsense.
            break
        x, err = cand, cand_err
    # Drop bands too small to matter (then the response is re-evaluated).
    peaks = []
    for k in range((len(x) - 4) // 3):
        pf, pg, pq = x[4 + 3 * k: 7 + 3 * k]
        if abs(pg) >= PEAK_MIN_GAIN_DB:
            peaks.append({"frequency_hz": float(2.0 ** pf), "gain_db": float(pg), "q": float(pq)})
    peaks.sort(key=lambda p: p["frequency_hz"])
    ls_g = float(x[1]) if abs(x[1]) >= SHELF_MIN_GAIN_DB else 0.0
    hs_g = float(x[3]) if abs(x[3]) >= SHELF_MIN_GAIN_DB else 0.0
    eq = {
        "low_shelf": {"frequency_hz": float(2.0 ** x[0]), "gain_db": ls_g, "q": SHELF_Q},
        "high_shelf": {"frequency_hz": float(2.0 ** x[2]), "gain_db": hs_g, "q": SHELF_Q},
        "peaks": peaks,
    }
    eq["response_db"] = eq_chain_response_db(eq, centers, sr)
    return eq


def eq_chain_response_db(eq: dict[str, Any], f: np.ndarray, sr: int) -> np.ndarray:
    h = np.zeros_like(f, dtype=float)
    if eq["low_shelf"]["gain_db"]:
        h = h + low_shelf_db(f, sr, eq["low_shelf"]["frequency_hz"], eq["low_shelf"]["gain_db"])
    if eq["high_shelf"]["gain_db"]:
        h = h + high_shelf_db(f, sr, eq["high_shelf"]["frequency_hz"], eq["high_shelf"]["gain_db"])
    for p in eq["peaks"]:
        h = h + peak_db(f, sr, p["frequency_hz"], p["gain_db"], p["q"])
    return h


# --- dynamics model ----------------------------------------------------------

def frame_stats(y: np.ndarray, sr: int) -> tuple[np.ndarray, np.ndarray, float]:
    """Per-frame (peak, mean power) of the signal and the frame length (s)."""
    x = y if y.ndim == 2 else y[:, None]
    hop = max(1, int(round(FRAME_SEC * sr)))
    n = x.shape[0] // hop
    if n > MAX_FRAMES:
        hop = int(math.ceil(x.shape[0] / MAX_FRAMES))
        n = x.shape[0] // hop
    n = max(n, 1)
    seg = x[: n * hop] if x.shape[0] >= hop else np.pad(x, ((0, hop - x.shape[0]), (0, 0)))
    seg = seg.reshape(n, -1, x.shape[1])
    peak = np.max(np.abs(seg), axis=(1, 2))
    power = np.mean(seg * seg, axis=(1, 2))
    return peak.astype(float), power.astype(float), hop / float(sr)


def compressor_gain_db(peak: np.ndarray, frame_sec: float, threshold_db: float, ratio: float,
                       attack_ms: float, release_ms: float) -> tuple[np.ndarray, np.ndarray]:
    """``(gr_db, peak_gr_db)`` per frame: gain reduction applied to the frame's
    body, and the (smaller) reduction its peak sees while the attack is still
    closing. Peak envelope with attack / release ballistics, then a hard-knee
    static curve - the shape of pedalboard's (JUCE) Compressor."""
    a_att = math.exp(-frame_sec * 1000.0 / max(attack_ms, 0.05))
    a_rel = math.exp(-frame_sec * 1000.0 / max(release_ms, 1.0))
    escape = 1.0 - math.exp(-5.0 / max(attack_ms, 0.05))   # share of GR a 5 ms transient sees
    slope = 1.0 - 1.0 / max(ratio, 1.0)
    level = 20.0 * np.log10(np.maximum(peak, 1e-9))
    gr = np.empty_like(level)
    pgr = np.empty_like(level)
    env = level[0]
    prev_gr = 0.0
    for i, lv in enumerate(level.tolist()):
        env = (a_att * env + (1.0 - a_att) * lv) if lv > env else (a_rel * env + (1.0 - a_rel) * lv)
        g = max(0.0, env - threshold_db) * slope
        gr[i] = g
        pgr[i] = prev_gr + (g - prev_gr) * escape if g > prev_gr else g
        prev_gr = g
    return gr, pgr


def model_crest_db(peak: np.ndarray, power: np.ndarray,
                   gr: np.ndarray | None = None, pgr: np.ndarray | None = None) -> float:
    p = peak if pgr is None else peak * 10.0 ** (-pgr / 20.0)
    pw = power if gr is None else power * 10.0 ** (-gr / 10.0)
    return float(20.0 * np.log10(np.max(p) + 1e-12) - 10.0 * np.log10(np.mean(pw) + 1e-20))


def model_lra_lu(power: np.ndarray, frame_sec: float, gr: np.ndarray | None = None) -> float:
    """EBU-3342-shaped LRA on the frame powers (3 s windows; unweighted, so
    only its *change* is used)."""
    pw = power if gr is None else power * 10.0 ** (-gr / 10.0)
    win = max(1, int(round(3.0 / frame_sec)))
    if pw.shape[0] < win + 1:
        return 0.0
    hop = max(1, int(round(0.1 / frame_sec)))
    c = np.concatenate([[0.0], np.cumsum(pw)])
    starts = np.arange(0, pw.shape[0] - win + 1, hop)
    st = 10.0 * np.log10((c[starts + win] - c[starts]) / win + 1e-20)
    st = st[st >= -70.0]
    if st.size < 2:
        return 0.0
    rel = 10.0 * np.log10(np.mean(10.0 ** (st / 10.0))) - 20.0
    st = np.sort(st[st >= rel])
    if st.size < 2:
        return 0.0
    def _rank(p: float) -> float:
        return float(st[min(st.size - 1, int(math.floor(p * (st.size - 1) + 0.5)))])
    return max(0.0, _rank(0.95) - _rank(0.10))


def fit_compressor(peak: np.ndarray, power: np.ndarray, frame_sec: float,
                   crest_delta_target: float | None, lra_delta_target: float | None
                   ) -> dict[str, Any] | None:
    """Fit threshold / ratio / attack / release so the modelled crest and LRA
    change by the target deltas (both must be reductions). None when no
    reduction is asked for."""
    want_crest = crest_delta_target is not None and crest_delta_target < -0.3
    want_lra = lra_delta_target is not None and lra_delta_target < -0.3
    if not (want_crest or want_lra):
        return None
    from scipy.optimize import differential_evolution
    crest0 = model_crest_db(peak, power)
    lra0 = model_lra_lu(power, frame_sec)
    top = 20.0 * math.log10(float(np.max(peak)) + 1e-12)
    floor = max(top - 30.0, -60.0)

    def _predict(v) -> tuple[float, float, np.ndarray]:
        gr, pgr = compressor_gain_db(peak, frame_sec, v[0], v[1], v[2], v[3])
        return (model_crest_db(peak, power, gr, pgr) - crest0,
                model_lra_lu(power, frame_sec, gr) - lra0, gr)

    def _cost(v) -> float:
        dc, dl, gr = _predict(v)
        c = 0.0
        if want_crest:
            c += (dc - crest_delta_target) ** 2
        if want_lra:
            c += 0.5 * (dl - lra_delta_target) ** 2
        mean_gr = float(np.mean(gr))
        c += 0.02 * (v[1] - 1.0) + (0.5 * (mean_gr - 6.0) ** 2 if mean_gr > 6.0 else 0.0)
        return c

    bounds = [(floor, min(top, 0.0)), (1.2, 8.0), (1.0, 60.0), (40.0, 600.0)]
    res = differential_evolution(_cost, bounds, seed=0, maxiter=25, popsize=6, tol=1e-3,
                                 polish=False)
    v = res.x
    dc, dl, gr = _predict(v)
    return {"threshold_db": float(v[0]), "ratio": float(v[1]), "attack_ms": float(v[2]),
            "release_ms": float(v[3]), "predicted_crest_delta_db": float(dc),
            "predicted_lra_delta_lu": float(dl), "mean_gain_reduction_db": float(np.mean(gr)),
            "_gr": gr}


# --- targets -----------------------------------------------------------------

def _measure(y: np.ndarray, sr: int) -> dict[str, Any]:
    from khensu_services.analysis.loudness import crest_factor_db, integrated_lufs, loudness_range
    centers, lv, power = band_levels_db(y, sr)
    chans = y.T if y.ndim == 2 else y
    return {"centers": centers, "levels": lv, "power": power,
            "crest_db": crest_factor_db(chans), "lra_lu": loudness_range(chans, sr),
            "lufs": integrated_lufs(chans, sr)}


def genre_tilt_target(centers: np.ndarray, power: np.ndarray, target_centroid_hz: float
                      ) -> tuple[np.ndarray, float]:
    """A dB/octave tilt about 1 kHz that moves the mix's spectral centroid to
    `target_centroid_hz` (clamped to +/-6 dB/oct)."""
    from scipy.optimize import brentq
    octs = np.log2(centers / 1000.0)

    def _centroid(slope: float) -> float:
        p = power * 10.0 ** (slope * octs / 10.0)
        return float(np.sum(centers * p) / np.sum(p))

    lo, hi = -6.0, 6.0
    f_lo, f_hi = _centroid(lo) - target_centroid_hz, _centroid(hi) - target_centroid_hz
    if f_lo > 0:
        slope = lo
    elif f_hi < 0:
        slope = hi
    else:
        slope = float(brentq(lambda s: _centroid(s) - target_centroid_hz, lo, hi, xtol=1e-3))
    return slope * octs, slope


def _r(v: float | None, nd: int = 2) -> float | None:
    return None if v is None or not math.isfinite(v) else round(float(v), nd)


def fit_chain(file_path: str, reference_path: str | None = None, genre: str | None = None,
              amount: float = 1.0, max_peaks: int = MAX_PEAKS) -> dict[str, Any]:
    """Fit the chain (no rendering). See the module docstring."""
    amount = float(min(1.0, max(0.0, amount)))
    y, sr = sf.read(file_path, dtype="float32", always_2d=True)
    mix = _measure(y, int(sr))
    centers, w = mix["centers"], band_weights(mix["centers"])
    notes: list[str] = []

    if reference_path:
        yr, srr = sf.read(reference_path, dtype="float32", always_2d=True)
        ref = _measure(yr, int(srr))
        ref_levels = np.interp(np.log2(centers), np.log2(ref["centers"]), ref["levels"])
        full_delta = level_free(ref_levels - mix["levels"], w)
        crest_goal = ref["crest_db"]
        lra_goal = ref["lra_lu"]
        target_info = {"kind": "reference", "label": reference_path}
    else:
        from khensu_services.util.genre_schema import load_genre_profile
        profile = load_genre_profile(genre or "") if genre else None
        if not profile:
            raise ValueError(f"Unknown genre: {genre!r}")
        centroid = float(profile.get("spectral_centroid_hz") or 0.0)
        if centroid > 0:
            full_delta, slope = genre_tilt_target(centers, mix["power"], centroid)
            full_delta = level_free(full_delta, w)
            notes.append(f"Genre norms carry a spectral centroid ({centroid:.0f} Hz), not a band "
                         f"spectrum: the EQ target is a {slope:+.2f} dB/octave tilt that moves "
                         "the mix's centroid there.")
        else:
            full_delta = np.zeros_like(centers)
        crest_goal = None
        lra_range = profile.get("lra_range")
        lra_goal = None
        if isinstance(lra_range, list) and len(lra_range) == 2 and mix["lra_lu"] is not None:
            lra_goal = float(min(max(mix["lra_lu"], lra_range[0]), lra_range[1]))
        target_info = {"kind": "genre", "label": str(profile.get("name") or genre)}

    target = amount * full_delta
    eq = fit_eq(centers, target, int(sr), max_peaks=max_peaks)
    eq_resp = np.asarray(eq.pop("response_db"))

    crest_delta = None if crest_goal is None or mix["crest_db"] is None \
        else amount * (crest_goal - mix["crest_db"])
    lra_delta = None if lra_goal is None or mix["lra_lu"] is None \
        else amount * (lra_goal - mix["lra_lu"])
    if crest_delta is not None and crest_delta > 0.3:
        notes.append(f"The target is {crest_delta / max(amount, 1e-9):.1f} dB more dynamic (crest) than the "
                     "mix; a compressor cannot add dynamics, so none is fitted for crest. A transient "
                     "shaper or a lighter master bus would be the move.")
    if lra_delta is not None and lra_delta > 0.3:
        notes.append("The target has a wider loudness range than the mix; a compressor cannot widen "
                     "it, so the LRA is left alone.")

    peak, power, frame_sec = frame_stats(y, int(sr))
    comp = fit_compressor(peak, power, frame_sec, crest_delta, lra_delta)
    gr = comp.pop("_gr") if comp else None

    # Makeup: keep integrated loudness where it was (EQ energy change from the
    # band powers, compressor loss from the frame model). The renderer's clip
    # guard keeps true peak under -1 dBTP regardless.
    eq_change = 10.0 * math.log10(float(np.sum(mix["power"] * 10.0 ** (eq_resp / 10.0)))
                                  / float(np.sum(mix["power"]) + 1e-20) + 1e-20)
    comp_change = 0.0
    if gr is not None:
        comp_change = 10.0 * math.log10(float(np.mean(power * 10.0 ** (-gr / 10.0)))
                                        / float(np.mean(power) + 1e-20) + 1e-20)
    makeup = float(np.clip(-(eq_change + comp_change), -12.0, 12.0))

    effects: list[dict[str, Any]] = []
    if eq["low_shelf"]["gain_db"]:
        effects.append({"effect": "LowShelfFilter", "parameters": {
            "cutoff_frequency_hz": eq["low_shelf"]["frequency_hz"], "gain_db": eq["low_shelf"]["gain_db"],
            "q": SHELF_Q}})
    for p in eq["peaks"]:
        effects.append({"effect": "PeakFilter", "parameters": {
            "cutoff_frequency_hz": p["frequency_hz"], "gain_db": p["gain_db"], "q": p["q"]}})
    if eq["high_shelf"]["gain_db"]:
        effects.append({"effect": "HighShelfFilter", "parameters": {
            "cutoff_frequency_hz": eq["high_shelf"]["frequency_hz"], "gain_db": eq["high_shelf"]["gain_db"],
            "q": SHELF_Q}})
    if comp:
        effects.append({"effect": "Compressor", "parameters": {
            k: comp[k] for k in ("threshold_db", "ratio", "attack_ms", "release_ms")}})
    if abs(makeup) >= 0.1:
        effects.append({"effect": "Gain", "parameters": {"gain_db": makeup}})
    for e in effects:
        e["parameters"] = {k: round(float(v), 2) for k, v in e["parameters"].items()}

    predicted_delta = level_free(eq_resp, w)
    before = {
        "band_error_db": _r(_wrms(target, w)),
        "crest_db": _r(mix["crest_db"]), "lra_lu": _r(mix["lra_lu"]), "lufs": _r(mix["lufs"]),
        "crest_error_db": _r(None if crest_delta is None else -crest_delta),
        "lra_error_lu": _r(None if lra_delta is None else -lra_delta),
    }
    pc = mix["crest_db"] + comp["predicted_crest_delta_db"] if comp and mix["crest_db"] is not None \
        else mix["crest_db"]
    pl = mix["lra_lu"] + comp["predicted_lra_delta_lu"] if comp and mix["lra_lu"] is not None \
        else mix["lra_lu"]
    predicted = {
        "band_error_db": _r(_wrms(level_free(predicted_delta - target, w), w)),
        "crest_db": _r(pc), "lra_lu": _r(pl), "lufs": _r(mix["lufs"]),
        "crest_error_db": _r(None if crest_delta is None or pc is None
                             else pc - (mix["crest_db"] + crest_delta)),
        "lra_error_lu": _r(None if lra_delta is None or pl is None
                           else pl - (mix["lra_lu"] + lra_delta)),
    }
    chain = {
        "low_shelf": {k: round(v, 2) for k, v in eq["low_shelf"].items()},
        "high_shelf": {k: round(v, 2) for k, v in eq["high_shelf"].items()},
        "peaks": [{k: round(v, 2) for k, v in p.items()} for p in eq["peaks"]],
        "compressor": None if not comp else {
            k: round(comp[k], 2) for k in ("threshold_db", "ratio", "attack_ms", "release_ms",
                                           "mean_gain_reduction_db")},
        "makeup_gain_db": round(makeup, 2),
    }
    return {
        "amount": amount,
        "target": target_info,
        "chain": chain,
        "effects": effects,
        "band_centers_hz": [round(float(c), 1) for c in centers],
        "target_delta_db": [round(float(v), 2) for v in target],
        "predicted_delta_db": [round(float(v), 2) for v in predicted_delta],
        "before": before,
        "predicted": predicted,
        "notes": notes,
        # For measure_render (not part of the wire response).
        "_mix": mix, "_target": target, "_crest_delta": crest_delta, "_lra_delta": lra_delta,
    }


def measure_render(fit: dict[str, Any], output_path: str) -> dict[str, Any]:
    """The same errors as `predicted`, measured on the rendered file."""
    y, sr = sf.read(output_path, dtype="float32", always_2d=True)
    out = _measure(y, int(sr))
    mix = fit["_mix"]
    w = band_weights(mix["centers"])
    out_levels = np.interp(np.log2(mix["centers"]), np.log2(out["centers"]), out["levels"])
    measured_delta = level_free(out_levels - mix["levels"], w)
    target = fit["_target"]
    cd, ld = fit["_crest_delta"], fit["_lra_delta"]
    return {
        "measured": {
            "band_error_db": _r(_wrms(level_free(measured_delta - target, w), w)),
            "crest_db": _r(out["crest_db"]), "lra_lu": _r(out["lra_lu"]), "lufs": _r(out["lufs"]),
            "crest_error_db": _r(None if cd is None or out["crest_db"] is None or mix["crest_db"] is None
                                 else out["crest_db"] - (mix["crest_db"] + cd)),
            "lra_error_lu": _r(None if ld is None or out["lra_lu"] is None or mix["lra_lu"] is None
                               else out["lra_lu"] - (mix["lra_lu"] + ld)),
        },
        "measured_delta_db": [round(float(v), 2) for v in measured_delta],
    }


def public(fit: dict[str, Any]) -> dict[str, Any]:
    return {k: v for k, v in fit.items() if not k.startswith("_")}


def fit_and_render(file_path: str, reference_path: str | None, genre: str | None,
                   amount: float, max_peaks: int, output_path: str | None,
                   render_fn: Callable[[list[dict], str], dict] | None) -> dict[str, Any]:
    """Fit, then (when `output_path` and `render_fn` are given) render the
    chain and measure the result. `render_fn(effects, output_path)` returns a
    dict with a ``render_state`` ("validated" on success) and optionally
    ``render_error``."""
    fit = fit_chain(file_path, reference_path, genre, amount, max_peaks)
    out = public(fit)
    if output_path and render_fn is not None:
        if not fit["effects"]:
            out["render_state"] = "skipped"
            out["notes"] = out["notes"] + ["Nothing to change at this amount; no render."]
            return out
        rendered = render_fn(fit["effects"], output_path)
        out.update({k: v for k, v in rendered.items() if v is not None})
        if rendered.get("render_state") == "validated":
            out["output_path"] = output_path
            out.update(measure_render(fit, output_path))
    return out
