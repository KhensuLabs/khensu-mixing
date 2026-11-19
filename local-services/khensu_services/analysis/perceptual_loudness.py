# local-services/khensu_services/analysis/perceptual_loudness.py
"""Perceptual loudness (CE-AI-5): Moore-Glasberg specific loudness on an
ERB-rate excitation pattern, and the PARTIAL loudness of a target signal heard
in the presence of a masker.

"The vocal is buried" becomes a number: how loud the vocal is on its own
(sones), how loud it still is inside the mix (partial loudness, sones), and
the ratio between the two - per band and over time.

Model (after Moore, Glasberg & Baer 1997, J. Audio Eng. Soc. 45(4), and the
ANSI S3.4-2007 formulation; time-varying smoothing after Glasberg & Moore
2002, J. Audio Eng. Soc. 50(5)):

1. Power spectrum per frame (Hann, 4096 points scaled to the sample rate),
   calibrated to dB SPL (see *Calibration* below).
2. Outer + middle ear transmission ``T(f)``. Above 500 Hz it is the inverse
   of the free-field threshold (ISO 389-7 MAF), so a tone at threshold
   produces the constant threshold excitation ``E_THRQ`` = 3.63 dB of the
   model. Below 500 Hz the threshold rise is split, as in MGB97, between the
   middle-ear attenuation (the ANSI S3.4-2007 middle-ear table) and a rising
   ``E_THRQ`` (lower cochlear-amplifier gain ``G``).
3. Excitation pattern on an ERB-rate grid of 1.8..38.9 Cam in 0.1 Cam steps
   from rounded-exponential (roex) auditory filters,
   ``W(g) = (1 + p g) exp(-p g)``, ``p_u = 4 fc / ERB(fc)``; the lower skirt
   flattens with the component's level per ERB,
   ``p_l = p51 - 0.35 (p51 / p51_1k)(X - 51)`` (upward spread of masking).
4. Specific loudness ``N' = C [(G E + A)^a - A^a]`` (with the MGB97
   sub-threshold factor), ``a`` and ``A`` interpolated from ``G``; total
   loudness is ``N = 0.1 * sum N'`` (sones).
5. Partial loudness of a signal in a masker: the MGB97 equations with the
   threshold-in-noise ``E_THRN = K E_noise + E_THRQ``.
6. Time-varying: instantaneous loudness per frame, short-term loudness by an
   attack/release smoother (22 ms / 50 ms), long-term by 100 ms / 2000 ms.

Calibration convention (documented, tested):
  * ``full_scale_sine_db_spl`` (default 100): a full-scale digital sine
    (peak 1.0, RMS -3.01 dBFS) is taken to play at this SPL. So a 1 kHz sine
    of peak amplitude ``10 ** ((40 - 100) / 20)`` = 0.001 is a 40 dB SPL tone.
  * The loudness constant ``C`` is not hard-coded: it is solved at first use
    so that a 1 kHz, 40 dB SPL tone in quiet gives exactly 1.0 sone (the
    definition of the sone; the binaural/diotic factor is absorbed into C).
    Loudness level in phon is read off the model's own 1 kHz tone loudness
    function, so 40 dB SPL at 1 kHz is 40 phon by construction.
  * The constant tables below are interpolated from the published tables at
    reduced precision (about +-1 dB). The model is a faithful structure with
    approximate constants, not a certified ANSI S3.4 implementation; tests
    pin its anchors (1 sone at 40 phon, roughly doubling per 10 phon, more
    loudness for spectrally spread energy, less partial loudness under an
    overlapping masker than under a distant one).

Numpy-only; heavy work is vectorised per chunk of frames.
"""
from __future__ import annotations

import threading
from typing import Any

import numpy as np
import numpy.typing as npt

# --- Constants --------------------------------------------------------------

E_THRQ_500_DB = 3.63          # excitation at absolute threshold, f >= 500 Hz (MGB97)
CAM_STEP = 0.1
CAMS = np.round(np.arange(1.8, 38.9 + 1e-9, CAM_STEP), 4)
_COMP_STEP = 0.25            # spectral components are pooled on a 0.25-Cam grid
DEFAULT_FULL_SCALE_SINE_DB_SPL = 100.0
N_FFT_44K = 4096

# ISO 389-7 free-field minimum audible field (dB SPL), 20 Hz..16 kHz, with a
# steep extrapolation to 20 kHz.
_MAF_F = np.array([20, 25, 31.5, 40, 50, 63, 80, 100, 125, 160, 200, 250, 315, 400, 500,
                   630, 800, 1000, 1250, 1600, 2000, 2500, 3150, 4000, 5000, 6300, 8000,
                   10000, 12500, 14000, 16000, 18000, 20000], dtype=float)
_MAF_DB = np.array([78.5, 68.7, 59.5, 51.1, 44.0, 37.5, 31.5, 26.5, 22.1, 17.9, 14.4, 11.4,
                    8.6, 6.2, 4.4, 3.0, 2.2, 2.4, 3.5, 1.7, -1.3, -4.2, -6.0, -5.4, -1.5, 6.0,
                    12.6, 13.9, 12.3, 18.4, 40.2, 70.0, 90.0], dtype=float)

# ANSI S3.4-2007 middle-ear transfer below 500 Hz (dB, absolute), used for the
# low-frequency share of the threshold rise.
_ME_F = np.array([20, 25, 31.5, 40, 50, 63, 80, 100, 125, 160, 200, 250, 315, 400, 500], dtype=float)
_ME_DB = np.array([-39.6, -32.0, -25.85, -21.4, -18.5, -15.9, -14.1, -12.4, -11.0, -9.6,
                   -8.3, -7.4, -6.2, -4.8, -3.8], dtype=float)

# alpha and A as functions of the low-level gain G (MGB97 / ANSI S3.4 table).
_G_DB = np.array([-25.0, -20.0, -15.0, -10.0, -5.0, 0.0])
_ALPHA = np.array([0.267, 0.250, 0.235, 0.222, 0.211, 0.200])
_A = np.array([30.89, 21.62, 14.97, 10.27, 6.94, 4.62])

# K (dB): signal-to-masker excitation ratio at masked threshold (MGB97 fig. 9,
# approximate): larger at low frequencies, about -3 dB above 1 kHz.
_K_F = np.array([50.0, 100.0, 200.0, 300.0, 500.0, 700.0, 1000.0, 20000.0])
_K_DB = np.array([13.0, 8.0, 3.0, 0.5, -1.5, -2.5, -3.0, -3.0])

_X_LEVELS = np.arange(-10.0, 132.0, 2.0)   # dB/ERB grid for the level-dependent lower skirt
_MAX_FRAMES = 3000
_CHUNK = 128

# Time constants (Glasberg & Moore 2002).
_STL_ATTACK_S, _STL_RELEASE_S = 0.022, 0.050
_LTL_ATTACK_S, _LTL_RELEASE_S = 0.100, 2.000


def erb_hz(f: npt.ArrayLike) -> np.ndarray:
    """Equivalent rectangular bandwidth (Hz) of the auditory filter at ``f``."""
    return 24.673 * (0.004368 * np.asarray(f, dtype=float) + 1.0)


def hz_to_cam(f: npt.ArrayLike) -> np.ndarray:
    return 21.366 * np.log10(0.004368 * np.asarray(f, dtype=float) + 1.0)


def cam_to_hz(c: npt.ArrayLike) -> np.ndarray:
    return (10.0 ** (np.asarray(c, dtype=float) / 21.366) - 1.0) / 0.004368


FC = cam_to_hz(CAMS)


def _maf(f: npt.ArrayLike) -> np.ndarray:
    return np.interp(np.log10(np.maximum(np.asarray(f, dtype=float), 20.0)), np.log10(_MAF_F), _MAF_DB)


def transmission_db(f: npt.ArrayLike) -> np.ndarray:
    """Outer + middle ear gain (dB) from free-field SPL to cochlear excitation."""
    f = np.asarray(f, dtype=float)
    t500 = E_THRQ_500_DB - float(_maf(500.0))
    me = np.interp(np.log10(np.clip(f, 20.0, 500.0)), np.log10(_ME_F), _ME_DB)
    low = t500 + (me - _ME_DB[-1])
    high = E_THRQ_500_DB - _maf(f)
    return np.where(f < 500.0, low, high)


def e_thrq_db(f: npt.ArrayLike) -> np.ndarray:
    """Excitation (dB) at absolute threshold: constant above 500 Hz, rising below."""
    f = np.asarray(f, dtype=float)
    return np.where(f < 500.0, _maf(f) + transmission_db(f), E_THRQ_500_DB)


def _k_linear(f: npt.ArrayLike) -> np.ndarray:
    return 10.0 ** (np.interp(np.log10(np.clip(f, 50.0, 20000.0)), np.log10(_K_F), _K_DB) / 10.0)


# Per-channel constants on the Cam grid.
_ETHRQ = 10.0 ** (e_thrq_db(FC) / 10.0)
_G_DB_FC = E_THRQ_500_DB - e_thrq_db(FC)                 # <= 0 dB
_G = 10.0 ** (_G_DB_FC / 10.0)
_ALPHA_FC = np.interp(_G_DB_FC, _G_DB, _ALPHA)
_A_FC = np.interp(_G_DB_FC, _G_DB, _A)
_K = _k_linear(FC)


# --- Excitation -------------------------------------------------------------

class _Grid:
    """Spectral-component grid + roex weights for one (sample rate, n_fft)."""

    def __init__(self, sr: int, n_fft: int):
        self.sr, self.n_fft = sr, n_fft
        freqs = np.fft.rfftfreq(n_fft, 1.0 / sr)
        nyq_cam = float(hz_to_cam(min(sr / 2.0, 20000.0)))
        edges = np.arange(float(CAMS[0]) - _COMP_STEP / 2, min(float(CAMS[-1]), nyq_cam) + _COMP_STEP, _COMP_STEP)
        bin_cam = hz_to_cam(np.maximum(freqs, 1e-6))
        self.band_of_bin = np.digitize(bin_cam, edges) - 1
        self.n_comp = len(edges) - 1
        self.valid = (self.band_of_bin >= 0) & (self.band_of_bin < self.n_comp) & (freqs >= 20.0)
        self.freqs = freqs
        self.comp_hz = cam_to_hz(0.5 * (edges[:-1] + edges[1:]))
        # Transmission applied per FFT bin (linear power).
        self.bin_gain = 10.0 ** (transmission_db(np.maximum(freqs, 20.0)) / 10.0)
        fc = FC[None, :]
        p51 = 4.0 * FC / erb_hz(FC)
        p51_1k = 4.0 * 1000.0 / float(erb_hz(1000.0))
        g = (self.comp_hz[:, None] - fc) / fc               # [comp, fc]
        ag = np.abs(g)
        pu = np.broadcast_to(p51[None, :], g.shape)
        w_upper = (1.0 + pu * ag) * np.exp(-pu * ag)
        # Lower skirt per input level X: [n_levels, comp, fc]
        pl = p51[None, None, :] - 0.35 * (p51[None, None, :] / p51_1k) * (_X_LEVELS[:, None, None] - 51.0)
        pl = np.maximum(pl, 0.5)
        w_lower = (1.0 + pl * ag[None]) * np.exp(-pl * ag[None])
        below = (g < 0.0)[None]                              # component below fc -> lower skirt
        w = np.where(below, w_lower, w_upper[None])
        w[:, ag > 4.0] = 0.0
        self.weights = w.astype(np.float64)                  # [levels, comp, fc]

    def components(self, power: np.ndarray) -> np.ndarray:
        """[frames, bins] calibrated power -> [frames, comp] intensity."""
        p = power[:, self.valid] * self.bin_gain[self.valid]
        idx = self.band_of_bin[self.valid]
        out = np.zeros((power.shape[0], self.n_comp))
        for k in range(power.shape[0]):
            out[k] = np.bincount(idx, weights=p[k], minlength=self.n_comp)[: self.n_comp]
        return out

    def excitation(self, comp: np.ndarray) -> np.ndarray:
        """[frames, comp] intensity -> [frames, fc] excitation (linear)."""
        # Level per ERB around each component: +-0.5 Cam (5 taps of 0.25 Cam,
        # half-weight ends).
        k = np.array([0.5, 1.0, 1.0, 1.0, 0.5])
        per_erb = np.stack([np.convolve(row, k, mode="same") for row in comp]) if comp.size else comp
        x_db = 10.0 * np.log10(np.maximum(per_erb, 1e-12))
        x_idx = np.clip(np.rint((x_db - _X_LEVELS[0]) / 2.0).astype(int), 0, len(_X_LEVELS) - 1)
        cols = np.arange(comp.shape[1])
        out = np.empty((comp.shape[0], len(FC)))
        for f in range(comp.shape[0]):
            w = self.weights[x_idx[f], cols]                 # [comp, fc]
            out[f] = comp[f] @ w
        return out


_grid_cache: dict[tuple[int, int], _Grid] = {}
_grid_lock = threading.Lock()


def _grid(sr: int, n_fft: int) -> _Grid:
    key = (int(sr), int(n_fft))
    with _grid_lock:
        g = _grid_cache.get(key)
        if g is None:
            g = _Grid(*key)
            _grid_cache[key] = g
        return g


# --- Specific / partial loudness ---------------------------------------------

def _specific_raw(e: np.ndarray) -> np.ndarray:
    """MGB97 specific loudness with C = 1 (C is applied by the caller)."""
    a, A, G, thr = _ALPHA_FC, _A_FC, _G, _ETHRQ
    core = (G * e + A) ** a - A ** a
    sub = (2.0 * e / (e + thr)) ** 1.5
    return np.maximum(np.where(e < thr, sub * core, core), 0.0)


def _partial_raw(es: np.ndarray, en: np.ndarray) -> np.ndarray:
    """MGB97 partial specific loudness of a signal (excitation ``es``) in a
    masker (excitation ``en``), C = 1."""
    a, A, G, thrq, K = _ALPHA_FC, _A_FC, _G, _ETHRQ, _K
    es = np.maximum(es, 1e-30)
    thrn = K * en + thrq
    total = ((es + en) * G + A) ** a
    above = (total - A ** a) - (((en * (1.0 + K) + thrq) * G + A) ** a - (thrq * G + A) ** a) * (thrn / es) ** 0.3
    ratio_num = (thrq * G + A) ** a - A ** a
    ratio_den = ((en * (1.0 + K) + thrq) * G + A) ** a - (en * G + A) ** a
    below = ((2.0 * es / (es + thrn)) ** 1.5) * (ratio_num / np.maximum(ratio_den, 1e-30)) \
        * (total - (en * G + A) ** a)
    return np.maximum(np.where(es >= thrn, above, below), 0.0)


_calib_lock = threading.Lock()
_calib: dict[str, Any] = {}


def _tone_excitation(freq_hz: float, level_db_spl: float) -> np.ndarray:
    """Excitation of a single pure tone (one component), linear units."""
    intensity = 10.0 ** ((level_db_spl + float(transmission_db(freq_hz))) / 10.0)
    p51 = 4.0 * FC / erb_hz(FC)
    p51_1k = 4.0 * 1000.0 / float(erb_hz(1000.0))
    gg = (freq_hz - FC) / FC
    ag = np.abs(gg)
    x = 10.0 * np.log10(max(intensity, 1e-12))
    pl = np.maximum(p51 - 0.35 * (p51 / p51_1k) * (x - 51.0), 0.5)
    p = np.where(gg < 0.0, pl, p51)
    w = (1.0 + p * ag) * np.exp(-p * ag)
    w[ag > 4.0] = 0.0
    return intensity * w


def _calibration() -> dict[str, Any]:
    with _calib_lock:
        if not _calib:
            raw = 0.1 * float(_specific_raw(_tone_excitation(1000.0, 40.0)).sum())
            c = 1.0 / raw
            levels = np.arange(0.0, 121.0, 1.0)
            sones = np.array([c * 0.1 * float(_specific_raw(_tone_excitation(1000.0, lv)).sum())
                              for lv in levels])
            _calib.update(C=c, phon_levels=levels, phon_sones=np.maximum.accumulate(sones))
        return _calib


def loudness_constant() -> float:
    """The calibrated C (1 kHz / 40 dB SPL tone = 1 sone)."""
    return float(_calibration()["C"])


def sone_to_phon(sone: float) -> float:
    """Loudness level (phon) of ``sone`` on the model's own 1 kHz tone curve."""
    cal = _calibration()
    s = float(sone)
    if s <= 0.0:
        return 0.0
    return float(np.interp(s, cal["phon_sones"], cal["phon_levels"]))


def tone_loudness_sone(freq_hz: float, level_db_spl: float) -> float:
    """Loudness (sones) of a steady pure tone in quiet - the analytic path the
    calibration uses; handy for sanity checks."""
    return loudness_constant() * 0.1 * float(_specific_raw(_tone_excitation(freq_hz, level_db_spl)).sum())


# --- Signal analysis ---------------------------------------------------------

def _n_fft_for(sr: int) -> int:
    return int(2 ** np.round(np.log2(N_FFT_44K * sr / 44100.0)))


def _as_channels(y: npt.ArrayLike) -> np.ndarray:
    a = np.asarray(y, dtype=np.float64)
    if a.ndim == 1:
        return a[None, :]
    return a


def _frame_power(y: np.ndarray, sr: int, n_fft: int, hop: int,
                 full_scale_sine_db_spl: float) -> np.ndarray:
    """[frames, bins] power per bin in linear SPL units (10^(dB SPL/10)),
    averaged over channels (diotic presentation)."""
    chans = _as_channels(y)
    n = chans.shape[1]
    if n < n_fft:
        chans = np.pad(chans, ((0, 0), (0, n_fft - n)))
        n = n_fft
    n_frames = 1 + (n - n_fft) // hop
    win = np.hanning(n_fft)
    norm = 2.0 / (n_fft * float(np.sum(win ** 2)))
    offset = 10.0 ** ((full_scale_sine_db_spl + 10.0 * np.log10(2.0)) / 10.0)
    out = np.zeros((n_frames, n_fft // 2 + 1))
    for start in range(0, n_frames, _CHUNK):
        stop = min(n_frames, start + _CHUNK)
        idx = (np.arange(start, stop) * hop)[:, None] + np.arange(n_fft)[None, :]
        acc = np.zeros((stop - start, n_fft // 2 + 1))
        for ch in chans:
            spec = np.fft.rfft(ch[idx] * win, axis=1)
            acc += (spec.real ** 2 + spec.imag ** 2) * norm
        out[start:stop] = acc / chans.shape[0]
    out[:, 0] *= 0.5
    return out * offset


def _hop_for(n: int, sr: int, n_fft: int) -> int:
    hop = n_fft // 2
    frames = 1 + max(0, n - n_fft) // hop
    if frames > _MAX_FRAMES:
        hop = int(np.ceil(max(1, n - n_fft) / (_MAX_FRAMES - 1)))
    return hop


def _smooth(x: np.ndarray, hop_s: float, attack_s: float, release_s: float) -> np.ndarray:
    a_up = 1.0 - np.exp(-hop_s / attack_s)
    a_dn = 1.0 - np.exp(-hop_s / release_s)
    out = np.empty_like(x)
    prev = 0.0
    for i, v in enumerate(x):
        prev = prev + (a_up if v > prev else a_dn) * (v - prev)
        out[i] = prev
    return out


def excitation_frames(y: npt.ArrayLike, sr: int, *, hop: int | None = None,
                      full_scale_sine_db_spl: float = DEFAULT_FULL_SCALE_SINE_DB_SPL
                      ) -> tuple[np.ndarray, float]:
    """(excitation [frames, Cam], hop seconds) of a signal."""
    chans = _as_channels(y)
    n_fft = _n_fft_for(int(sr))
    hop = hop or _hop_for(chans.shape[1], int(sr), n_fft)
    g = _grid(int(sr), n_fft)
    power = _frame_power(chans, int(sr), n_fft, hop, full_scale_sine_db_spl)
    return g.excitation(g.components(power)), hop / float(sr)


def _summarise(inst: np.ndarray, hop_s: float) -> dict[str, float]:
    stl = _smooth(inst, hop_s, _STL_ATTACK_S, _STL_RELEASE_S)
    ltl = _smooth(stl, hop_s, _LTL_ATTACK_S, _LTL_RELEASE_S)
    mean = float(np.mean(stl)) if stl.size else 0.0
    return {
        "loudness_sone": mean,
        "loudness_phon": sone_to_phon(mean),
        "peak_short_term_sone": float(np.percentile(stl, 95)) if stl.size else 0.0,
        "max_long_term_sone": float(np.max(ltl)) if ltl.size else 0.0,
    }


def specific_loudness(y: npt.ArrayLike, sr: int,
                      full_scale_sine_db_spl: float = DEFAULT_FULL_SCALE_SINE_DB_SPL) -> dict[str, Any]:
    """Time-averaged specific loudness pattern (sone/Cam) and loudness summary
    of a signal in quiet."""
    exc, hop_s = excitation_frames(y, sr, full_scale_sine_db_spl=full_scale_sine_db_spl)
    c = loudness_constant()
    spec = c * _specific_raw(exc)
    inst = 0.1 * spec.sum(axis=1)
    out = _summarise(inst, hop_s)
    out["cams"] = CAMS.tolist()
    out["specific_loudness"] = spec.mean(axis=0).tolist() if spec.size else []
    return out


def loudness_of_signal(y: npt.ArrayLike, sr: int,
                       full_scale_sine_db_spl: float = DEFAULT_FULL_SCALE_SINE_DB_SPL) -> float:
    """Mean short-term loudness (sones) of a signal in quiet."""
    return float(specific_loudness(y, sr, full_scale_sine_db_spl)["loudness_sone"])


# Report bands: the masking analyzer's bands, so the two panels line up.
def _report_bands() -> list[tuple[float, float]]:
    from khensu_services.analysis.masking import _MASKING_BANDS
    return list(_MASKING_BANDS)


def _verdict(ratio: float) -> str:
    if ratio >= 0.7:
        return "clear"
    if ratio >= 0.4:
        return "competing"
    return "buried"


def partial_loudness(target: npt.ArrayLike, masker: npt.ArrayLike, sr: int, *,
                     full_scale_sine_db_spl: float = DEFAULT_FULL_SCALE_SINE_DB_SPL,
                     timeline_window_s: float = 1.0) -> dict[str, Any]:
    """Loudness of ``target`` alone and inside ``masker`` (both aligned arrays
    at ``sr``; channels-first or mono)."""
    t = _as_channels(target)
    m = _as_channels(masker)
    n = min(t.shape[1], m.shape[1])
    t, m = t[:, :n], m[:, :n]
    n_fft = _n_fft_for(int(sr))
    hop = _hop_for(n, int(sr), n_fft)
    es, hop_s = excitation_frames(t, sr, hop=hop, full_scale_sine_db_spl=full_scale_sine_db_spl)
    en, _ = excitation_frames(m, sr, hop=hop, full_scale_sine_db_spl=full_scale_sine_db_spl)
    c = loudness_constant()
    spec_alone = c * _specific_raw(es)
    spec_partial = c * _partial_raw(es, en)
    spec_masker = c * _specific_raw(en)
    inst_alone = 0.1 * spec_alone.sum(axis=1)
    inst_partial = 0.1 * spec_partial.sum(axis=1)
    inst_masker = 0.1 * spec_masker.sum(axis=1)

    alone = _summarise(inst_alone, hop_s)
    part = _summarise(inst_partial, hop_s)
    mask = _summarise(inst_masker, hop_s)
    ratio = part["loudness_sone"] / alone["loudness_sone"] if alone["loudness_sone"] > 1e-6 else 0.0
    ratio = float(np.clip(ratio, 0.0, 1.0))

    bands = []
    mean_alone = spec_alone.mean(axis=0)
    mean_part = spec_partial.mean(axis=0)
    for lo, hi in _report_bands():
        sel = (FC >= lo) & (FC < hi)
        if not sel.any():
            continue
        ua = 0.1 * float(mean_alone[sel].sum())
        pa = 0.1 * float(mean_part[sel].sum())
        bands.append({"lo_hz": float(lo), "hi_hz": float(hi), "unmasked_sone": ua,
                      "partial_sone": pa, "ratio": float(pa / ua) if ua > 1e-6 else 0.0})

    timeline = []
    per = max(1, int(round(timeline_window_s / hop_s)))
    for i in range(0, len(inst_alone), per):
        ua = float(np.mean(inst_alone[i:i + per]))
        pa = float(np.mean(inst_partial[i:i + per]))
        timeline.append({"start_s": round(i * hop_s, 3), "end_s": round(min(len(inst_alone), i + per) * hop_s, 3),
                         "unmasked_sone": ua, "partial_sone": pa,
                         "ratio": float(np.clip(pa / ua, 0.0, 1.0)) if ua > 1e-6 else 0.0})

    worst = min((b for b in bands if b["unmasked_sone"] > 0.05 * max(alone["loudness_sone"], 1e-6)),
                key=lambda b: b["ratio"], default=None)
    return {
        "target_loudness_sone": alone["loudness_sone"],
        "target_loudness_phon": alone["loudness_phon"],
        "target_partial_loudness_sone": part["loudness_sone"],
        "target_partial_loudness_phon": part["loudness_phon"],
        "masker_loudness_sone": mask["loudness_sone"],
        "masker_loudness_phon": mask["loudness_phon"],
        "partial_ratio": ratio,
        "masked_pct": round(100.0 * (1.0 - ratio), 1),
        "verdict": _verdict(ratio),
        "worst_band": worst,
        "bands": bands,
        "timeline": timeline,
        "frames": int(len(inst_alone)),
        "hop_s": float(hop_s),
    }


def _load(path: str, sr: int | None = None) -> tuple[np.ndarray, int]:
    from khensu_services.analysis.context import AnalysisContext
    y, s = AnalysisContext(path).load(sr=sr, mono=False)
    y = np.asarray(y, dtype=np.float64)
    if y.ndim == 1:
        y = y[None, :]
    return y, int(s)


def _match_channels(a: np.ndarray, b: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    if a.shape[0] == b.shape[0]:
        return a, b
    if a.shape[0] == 1:
        return np.repeat(a, b.shape[0], axis=0), b
    if b.shape[0] == 1:
        return a, np.repeat(b, a.shape[0], axis=0)
    k = min(a.shape[0], b.shape[0])
    return a[:k], b[:k]


def analyze_partial_loudness(target_path: str, mix_path: str | None = None,
                             masker_paths: list[str] | None = None, *,
                             full_scale_sine_db_spl: float = DEFAULT_FULL_SCALE_SINE_DB_SPL,
                             start_s: float | None = None, end_s: float | None = None,
                             timeline_window_s: float = 1.0) -> dict[str, Any]:
    """Partial loudness of the target stem in the mix.

    The masker is ``mix - target`` when ``mix_path`` is given (stems from the
    separation pipeline sum back to the mix), else the sum of
    ``masker_paths``. Inputs are aligned at sample 0, resampled to the
    target's rate and trimmed to the shortest."""
    target, sr = _load(target_path)
    if mix_path:
        mix, _ = _load(mix_path, sr=sr)
        target, mix = _match_channels(target, mix)
        n = min(target.shape[1], mix.shape[1])
        target = target[:, :n]
        masker = mix[:, :n] - target
        source = "mix_minus_target"
        # How well the target explains the mix: a target that is not in the
        # mix (wrong file, different gain) leaves a masker as loud as the mix.
        mix_e = float(np.mean(mix[:, :n] ** 2))
        resid_e = float(np.mean(masker ** 2))
        target_share_db = 10.0 * np.log10(max(mix_e, 1e-20) / max(resid_e, 1e-20))
    else:
        masker = None
        for p in masker_paths or []:
            m, _ = _load(p, sr=sr)
            target, m = _match_channels(target, m)
            if masker is None:
                masker = m
            else:
                masker, m = _match_channels(masker, m)
                k = min(masker.shape[1], m.shape[1])
                masker = masker[:, :k] + m[:, :k]
        if masker is None:
            # No masker given: the target in quiet. Partial loudness then
            # equals the unmasked loudness (ratio 1) - a well-defined answer
            # rather than an error; the endpoint still requires a masker.
            masker = np.zeros_like(target)
            source = "none"
        else:
            source = "stems"
        n = min(target.shape[1], masker.shape[1])
        target, masker = target[:, :n], masker[:, :n]
        target_share_db = None

    duration = target.shape[1] / float(sr)
    s0 = 0 if start_s is None else int(max(0.0, start_s) * sr)
    s1 = target.shape[1] if end_s is None else int(min(duration, end_s) * sr)
    if s1 <= s0:
        raise ValueError("region is empty (starts at or after the end of the audio)")
    # A requested region must be long enough to measure; a whole file that is
    # simply short (a 50 ms clip) is measured as it is.
    if (start_s is not None or end_s is not None) and s1 - s0 < int(0.25 * sr):
        raise ValueError("region shorter than 0.25 s")
    out = partial_loudness(target[:, s0:s1], masker[:, s0:s1], sr,
                           full_scale_sine_db_spl=full_scale_sine_db_spl,
                           timeline_window_s=timeline_window_s)
    if s0:
        for row in out["timeline"]:
            row["start_s"] = round(row["start_s"] + s0 / sr, 3)
            row["end_s"] = round(row["end_s"] + s0 / sr, 3)
    out.update({
        "masker_source": source,
        "target_share_db": target_share_db,
        "duration_s": float((s1 - s0) / sr),
        "sample_rate": int(sr),
        "calibration": {
            "full_scale_sine_db_spl": float(full_scale_sine_db_spl),
            "reference": "1 kHz pure tone at 40 dB SPL = 1 sone = 40 phon",
            "model": "Moore-Glasberg-Baer 1997 partial loudness, Glasberg-Moore 2002 time smoothing "
                     "(approximate constants)",
        },
    })
    return out
