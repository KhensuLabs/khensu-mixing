# local-services/khensu_services/analysis/extended_analyzer.py
"""
Extended audio analysis: harshness, sibilance, mono compatibility, translation risk, resonance.
All functions are stateless and take numpy arrays.
"""
from __future__ import annotations
import numpy as np
import numpy.typing as npt
from typing import Any

from khensu_services.analysis.dsp_metrics import stereo_correlation, stereo_width


# ---------------------------------------------------------------------------
# Frequency band definitions for mono compat analysis
# ---------------------------------------------------------------------------
_BANDS: dict[str, tuple[float, float]] = {
    "sub":       (20.0,   80.0),
    "bass":      (80.0,  250.0),
    "midrange":  (250.0, 2000.0),
    "upper_mid": (2000.0, 6000.0),
    "air":       (6000.0, 20000.0),
}


def _bandpass(y: npt.NDArray, sr: int, low_hz: float, high_hz: float) -> npt.NDArray:
    """Causal (``sosfilt``, NOT zero-phase) 4th-order Butterworth band-pass.
    Returns a mono array. Only band RMS ratios are taken from it, so the
    filter's phase delay does not matter. (DSP-14)

    Accepts mono (1-D) or multi-channel (2-D shape: channels × samples).
    Always reduces to mono via mean(axis=0) before filtering.
    """
    from scipy import signal as sig  # lazy: scipy.signal is ~0.9 s of service start-up (PERF-4)

    nyq = sr * 0.5
    low = max(low_hz / nyq, 1e-6)
    high = min(high_hz / nyq, 1.0 - 1e-6)
    if low >= high:
        return np.zeros(y.shape[-1])
    sos = sig.butter(4, [low, high], btype="band", output="sos")
    mono = y.mean(axis=0) if y.ndim == 2 else y
    return np.asarray(sig.sosfilt(sos, mono))


def _amplitude_norm(window: npt.NDArray) -> float:
    """|rfft| divisor that maps a sinusoid of peak amplitude A to A (so
    20*log10 of the result is dBFS: a full-scale sine reads 0 dB)."""
    return float(np.sum(window)) / 2.0


def _averaged_spectrum(mono: npt.NDArray, sr: int, n_fft: int = 4096) -> tuple[npt.NDArray, npt.NDArray]:
    """Return (freqs, averaged magnitude spectrum) over the full track using
    overlapping windows, normalised to sinusoid peak amplitude (dBFS-ready).

    The magnitude is the RMS over frames, ``sqrt(mean |X|^2)`` from the
    shared ``dsp_metrics.averaged_power_spectrum`` - the power average, so a
    fluctuating component is not under-weighted (DSP-15). A steady sinusoid
    reads the same as before."""
    from khensu_services.analysis.dsp_metrics import averaged_power_spectrum

    spectrum = np.sqrt(averaged_power_spectrum(mono, n_fft))
    freqs = np.fft.rfftfreq(n_fft, d=1.0 / sr)
    return freqs, spectrum / _amplitude_norm(np.hanning(n_fft))


# ---------------------------------------------------------------------------
# Harshness detection (2.5–5.0 kHz transient energy)
# ---------------------------------------------------------------------------
def analyze_harshness(y: npt.NDArray, sr: int) -> dict[str, Any]:
    """
    Detect 2.5–5 kHz harshness by measuring the ratio of upper-mid energy
    (2.5–5 kHz) to overall mid energy (500 Hz–8 kHz).

    Peak frequency is identified by averaging the FFT magnitude spectrum over
    the **full track** (not just the first window), so a harsh chorus that
    occurs in the middle of the file is correctly detected.

    Returns:
        harshness_score: float 0–1 (1 = very harsh)
        peak_hz: float | None — dominant harsh frequency if score > 0.2
        peak_db: float | None — dB level of dominant peak
    """
    mono = y.mean(axis=0) if y.ndim == 2 else y
    harsh_band = _bandpass(y, sr, 2500.0, 5000.0)
    mid_band   = _bandpass(y, sr,  500.0, 8000.0)

    rms_harsh = float(np.sqrt(np.mean(harsh_band ** 2)) + 1e-12)
    rms_mid   = float(np.sqrt(np.mean(mid_band   ** 2)) + 1e-12)

    ratio = rms_harsh / rms_mid
    harshness_score = float(np.clip(ratio * 2.5, 0.0, 1.0))

    peak_hz: float | None = None
    peak_db: float | None = None

    if harshness_score > 0.2:
        # Average FFT over the full track to find the dominant harsh frequency.
        freqs, spectrum = _averaged_spectrum(mono, sr, n_fft=4096)
        mask = (freqs >= 2500) & (freqs <= 5000)
        if mask.any():
            sub = spectrum[mask]
            sub_freqs = freqs[mask]
            idx = int(np.argmax(sub))
            peak_hz = float(sub_freqs[idx])
            peak_db = float(20.0 * np.log10(sub[idx] + 1e-12))

    return {
        "score": harshness_score,
        "peak_hz": peak_hz,
        "peak_db": peak_db,
    }


def windowed_harshness_curve(y: npt.NDArray, sr: int,
                             block_sec: float = 3.0, hop_sec: float = 1.0) -> list[float]:
    """Per-window harshness score (same 2.5-5 kHz / 500 Hz-8 kHz ratio as
    analyze_harshness, windowed) so a harsh chorus is localizable instead of
    being averaged into one whole-track scalar (pipeline #6). Same block/hop as
    short_term_loudness_curve, so the series align window-for-window. The mean
    of this curve tracks the scalar analyze_harshness score."""
    harsh = _bandpass(y, sr, 2500.0, 5000.0)
    mid = _bandpass(y, sr, 500.0, 8000.0)
    # Same window grid as loudness.short_term_loudness_series.
    block = max(1, int(round(block_sec * sr)))
    hop = max(1, int(round(hop_sec * sr)))
    n = harsh.shape[0]

    # Digital silence is judged on the input (the band-pass filters ring on
    # past the end of content, so their output alone can't tell).
    x = y if y.ndim == 2 else y[np.newaxis, :]
    nz = np.concatenate([[0], np.cumsum(np.any(x != 0.0, axis=0))])

    def _score(s0: int, s1: int) -> float:
        rm = float(np.sqrt(np.mean(mid[s0:s1] ** 2)))
        if nz[s1] == nz[s0] or rm < 1e-9:   # silent window: 0, not "max harsh"
            return 0.0
        rh = float(np.sqrt(np.mean(harsh[s0:s1] ** 2)))
        return float(np.clip(rh / rm * 2.5, 0.0, 1.0))

    if n < block:
        return [_score(0, n)]
    return [_score(s0, s0 + block) for s0 in range(0, n - block + 1, hop)]


# ---------------------------------------------------------------------------
# Sibilance detection (5–10 kHz)  — separate from harshness (2.5–5 kHz)
# ---------------------------------------------------------------------------
def analyze_sibilance(y: npt.NDArray, sr: int) -> dict[str, Any]:
    """
    Detect sibilance energy in the 5–10 kHz de-essing zone, relative to the
    broader 4–12 kHz air band.

    A high score indicates 5–10 kHz energy is disproportionately loud, which
    typically causes listener fatigue on modern headphones and bright playback
    systems, and requires de-essing on vocal tracks.

    Returns:
        score: float 0–1 (1 = highly sibilant)
        peak_hz: float | None — dominant sibilant frequency when score > 0.25
    """
    if sr < 12000:
        return {"score": 0.0, "peak_hz": None}

    sib_band = _bandpass(y, sr, 5000.0, 10000.0)
    air_band = _bandpass(y, sr, 4000.0, 12000.0)

    rms_sib = float(np.sqrt(np.mean(sib_band ** 2)) + 1e-12)
    rms_air = float(np.sqrt(np.mean(air_band ** 2)) + 1e-12)

    ratio = rms_sib / rms_air
    sibilance_score = float(np.clip(ratio * 2.0, 0.0, 1.0))

    peak_hz: float | None = None
    if sibilance_score > 0.25:
        mono = y.mean(axis=0) if y.ndim == 2 else y
        freqs, spectrum = _averaged_spectrum(mono, sr, n_fft=4096)
        mask = (freqs >= 5000) & (freqs <= 10000)
        if mask.any():
            sub = spectrum[mask]
            sub_freqs = freqs[mask]
            peak_hz = float(sub_freqs[int(np.argmax(sub))])

    return {"score": sibilance_score, "peak_hz": peak_hz}


# ---------------------------------------------------------------------------
# Mono compatibility — per-band stereo correlation
# ---------------------------------------------------------------------------
def analyze_mono_compat(y: npt.NDArray, sr: int) -> dict[str, Any]:
    """
    Compute L/R stereo correlation per frequency band.

    Uses separate left and right channel bandpass paths so the Pearson
    correlation reflects actual L-vs-R decorrelation, not (L+R)/2 vs R.

    Returns:
        per_band_correlation: dict[band_name, float -1..1]
        low_end_stereo_pct: float 0–100, a low-end DECORRELATION index
            ``(1 - mean(sub, bass correlation)) * 50``: 0 = the low end is
            fully correlated (mono), 50 = uncorrelated, 100 = anti-phase. It
            is not a percentage of energy. (DSP-14)
    """
    per_band: dict[str, float] = {}
    for band_name, (low, high) in _BANDS.items():
        if y.ndim == 2 and y.shape[0] >= 2:
            # Pass each channel as a (1, N) array so _bandpass returns a mono filtered signal
            # for that channel only — NOT a downmix of both channels.
            bl = _bandpass(y[:1], sr, low, high)  # left channel
            br = _bandpass(y[1:], sr, low, high)  # right channel
        else:
            bl = _bandpass(y, sr, low, high)
            br = bl

        # Shared Pearson with a silence guard: a band with no content in
        # either channel can't cancel in mono (1.0) — the old epsilon form
        # read silence as 0 correlation, i.e. a mono-compat problem.
        per_band[band_name] = stereo_correlation(bl, br)

    sub_corr  = per_band["sub"]
    bass_corr = per_band["bass"]
    avg_low   = (sub_corr + bass_corr) / 2.0
    low_end_stereo_pct = float(np.clip((1.0 - avg_low) * 50.0, 0.0, 100.0))

    return {
        "per_band_correlation": per_band,
        "low_end_stereo_pct": low_end_stereo_pct,
    }


# ---------------------------------------------------------------------------
# Translation risk — per playback environment
# ---------------------------------------------------------------------------
def analyze_translation_risk(
    mono_result: dict[str, Any],
    spectral_result: dict[str, Any],
    dynamics_result: dict[str, Any],
) -> dict[str, Any]:
    """
    Estimate mix translation risk for 5 playback environments.
    Each score is 0–1 where 1 = high risk (poor translation).

    Environments: studio, earbuds, phone, car, club
    """
    per_band = mono_result.get("per_band_correlation", {})
    centroid_hz   = float(spectral_result.get("centroid_hz", 3000.0))
    balance_low   = float(spectral_result.get("balance_low", 0.35))
    crest_factor  = float(dynamics_result.get("crest_factor_db", 10.0))

    upper_mid_corr = per_band.get("upper_mid", 1.0)
    sub_corr       = per_band.get("sub", 1.0)
    bass_corr      = per_band.get("bass", 1.0)
    low_stereo_pct = mono_result.get("low_end_stereo_pct", 0.0)

    def clamp01(v: float) -> float:
        return max(0.0, min(1.0, v))

    avg_corr    = sum(per_band.values()) / max(len(per_band), 1)
    studio_risk = clamp01((1.0 - avg_corr) * 0.5)

    harsh_risk   = clamp01((centroid_hz - 3000.0) / 3000.0) if centroid_hz > 3000 else 0.0
    missing_sub  = clamp01(0.35 - balance_low) * 2.0 if balance_low < 0.35 else 0.0
    earbuds_risk = clamp01((harsh_risk * 0.5) + (missing_sub * 0.3) + ((1.0 - upper_mid_corr) * 0.2))

    excess_bass  = clamp01((balance_low - 0.40) * 3.0) if balance_low > 0.40 else 0.0
    mono_problem = clamp01((1.0 - (bass_corr + 1.0) / 2.0))
    phone_risk   = clamp01((excess_bass * 0.5) + (mono_problem * 0.3) + ((1.0 - upper_mid_corr) * 0.2))

    weak_low             = clamp01((0.30 - balance_low) * 4.0) if balance_low < 0.30 else 0.0
    over_compressed_risk = clamp01((6.0 - crest_factor) / 6.0) if crest_factor < 6.0 else 0.0
    car_risk             = clamp01((weak_low * 0.5) + (over_compressed_risk * 0.3) + (low_stereo_pct / 100.0 * 0.2))

    weak_sub_risk = clamp01((0.30 - balance_low) * 3.0) if balance_low < 0.30 else 0.0
    sub_mono_risk = clamp01((1.0 - sub_corr) * 0.5)
    club_risk     = clamp01((weak_sub_risk * 0.5) + (sub_mono_risk * 0.3) + (over_compressed_risk * 0.2))

    per_env: dict[str, float] = {
        "studio":  round(studio_risk, 3),
        "earbuds": round(earbuds_risk, 3),
        "phone":   round(phone_risk, 3),
        "car":     round(car_risk, 3),
        "club":    round(club_risk, 3),
    }
    return {
        "per_environment": per_env,
        "score": float(np.mean(list(per_env.values()))),
    }


# ---------------------------------------------------------------------------
# Vocal clarity / presence analysis
# ---------------------------------------------------------------------------
def analyze_vocal_clarity(y: npt.NDArray, sr: int) -> dict[str, Any]:
    """
    Estimate vocal presence and clarity from spectral energy distribution.

    This is a spectral proxy — not true vocal analysis. It measures how much
    energy lives in the 1–5 kHz vocal presence zone versus the 300 Hz–1 kHz
    mud zone. A high presence score with good clarity indicates a mix where
    vocals and upper-mid instruments have space and cut through.

    For true vocal-specific analysis, stem separation (HPSS or Demucs) is
    required. This function provides a fast approximation without stem separation.

    Returns:
        presence_score: 0–1 (presence zone energy relative to full mid band; 1 = well-represented)
        clarity_score:  0–1 (presence-to-mud ratio; 1 = vocal cuts cleanly through mud)
        presence_ratio: raw 1–5 kHz / 200 Hz–8 kHz energy ratio
        mud_ratio:      raw 300 Hz–1 kHz / 200 Hz–8 kHz energy ratio
    """
    pres_band = _bandpass(y, sr, 1000.0, 5000.0)
    mud_band  = _bandpass(y, sr,  300.0, 1000.0)
    full_band = _bandpass(y, sr,  200.0, 8000.0)

    rms_pres = float(np.sqrt(np.mean(pres_band ** 2)) + 1e-12)
    rms_mud  = float(np.sqrt(np.mean(mud_band  ** 2)) + 1e-12)
    rms_full = float(np.sqrt(np.mean(full_band ** 2)) + 1e-12)

    presence_ratio = rms_pres / rms_full
    mud_ratio      = rms_mud  / rms_full

    # 0.25 presence ratio is a reference for a balanced vocal-forward mix → score = 1.0
    presence_score = float(np.clip(presence_ratio / 0.25, 0.0, 1.0))
    # Clarity: how well presence energy dominates over mud (>1 = vocal cuts through)
    clarity_score  = float(np.clip((rms_pres / (rms_mud + 1e-12) - 0.5) / 1.5, 0.0, 1.0))

    return {
        "presence_score": round(presence_score, 3),
        "clarity_score":  round(clarity_score, 3),
        "presence_ratio": round(presence_ratio, 4),
        "mud_ratio":      round(mud_ratio, 4),
    }


# ---------------------------------------------------------------------------
# Mid-side frequency imbalance analysis
# ---------------------------------------------------------------------------
def analyze_mid_side_imbalance(y: npt.NDArray, sr: int) -> dict[str, Any]:
    """Detect spectral imbalance between mid (L+R)/2 and side (L-R)/2 channels.

    Computes whether the side channel carries a disproportionate amount of:
      - High-frequency energy above 4 kHz (side-channel brightness)
      - Sub-bass energy below 100 Hz (sub-bass width — should be near mono)

    Returns:
        side_bright_ratio: side/mid energy ratio above 4 kHz (>1.5 = side too bright)
        side_heavy_ratio:  side/mid energy ratio below 100 Hz (>0.3 = sub too wide)
        brightness_imbalance_db: dB difference (side − mid) in high band
        heaviness_imbalance_db:  dB difference (side − mid) in low band
        imbalance_severity: 0–1 composite severity
        warning: human-readable message when severity is notable, else None
    """
    if y.ndim < 2 or y.shape[0] < 2:
        return {
            "side_bright_ratio": 1.0, "side_heavy_ratio": 0.0,
            "brightness_imbalance_db": 0.0, "heaviness_imbalance_db": 0.0,
            "imbalance_severity": 0.0, "warning": None,
        }

    from khensu_services.analysis.dsp_metrics import averaged_power_spectrum

    left  = y[0].astype(np.float64)
    right = y[1].astype(np.float64)
    mid   = (left + right) * 0.5
    side  = (left - right) * 0.5

    # Frame-averaged power spectra (bounded memory) instead of two
    # full-length rFFTs (~230 MB each for 10 min at 48 kHz); 8192-point
    # frames keep ~6 Hz bins for the 20-100 Hz band. The "high" band stops at
    # 20 kHz so a 96 kHz file's ultrasonic side noise is not counted. (DSP-14)
    n_fft = 8192
    freqs = np.fft.rfftfreq(n_fft, d=1.0 / sr)
    mid_pow = averaged_power_spectrum(mid, n_fft)
    side_pow = averaged_power_spectrum(side, n_fft)

    def _band_energy(power: npt.NDArray, lo: float, hi: float) -> float:
        mask = (freqs >= lo) & (freqs < hi)
        return float(np.sum(power[mask]) + 1e-12)

    top = min(sr / 2.0, 20000.0)
    mid_high  = _band_energy(mid_pow,  4000.0, top)
    side_high = _band_energy(side_pow, 4000.0, top)
    mid_low   = _band_energy(mid_pow,  20.0,   100.0)
    side_low  = _band_energy(side_pow, 20.0,   100.0)

    bright_ratio = side_high / (mid_high + 1e-12)
    heavy_ratio  = side_low  / (mid_low  + 1e-12)

    brightness_db = float(10.0 * np.log10(side_high / (mid_high + 1e-12)))
    heaviness_db  = float(10.0 * np.log10(side_low  / (mid_low  + 1e-12)))

    # Severity: bright_ratio > 1.0 and heavy_ratio > 0.2 are penalty thresholds
    bright_severity = float(np.clip((bright_ratio - 1.0) / 2.0, 0.0, 1.0))
    heavy_severity  = float(np.clip((heavy_ratio  - 0.2) / 0.8, 0.0, 1.0))
    severity = float(np.clip(max(bright_severity, heavy_severity), 0.0, 1.0))

    warning: str | None = None
    if heavy_ratio > 0.3:
        warning = (
            f"Sub-bass is too wide (side/mid energy ratio = {heavy_ratio:.2f}); "
            "mono compatibility risk below 100 Hz."
        )
    elif bright_ratio > 1.8:
        warning = (
            f"Side channel is significantly brighter than mid (ratio = {bright_ratio:.2f}); "
            "may sound harsh on mono playback."
        )

    return {
        "side_bright_ratio":       round(float(bright_ratio), 3),
        "side_heavy_ratio":        round(float(heavy_ratio), 3),
        "brightness_imbalance_db": round(brightness_db, 1),
        "heaviness_imbalance_db":  round(heaviness_db, 1),
        "imbalance_severity":      round(severity, 3),
        "warning":                 warning,
    }


# ---------------------------------------------------------------------------
# Resonance hotspot detection
# ---------------------------------------------------------------------------
def analyze_resonances(y: npt.NDArray, sr: int,
                        max_hotspots: int = 5) -> dict[str, Any]:
    """
    Detect resonance hotspots (narrow spectral peaks with prominence >= 8 dB).
    Returns a dict with key "hotspots": list of at most `max_hotspots` peaks,
    sorted by prominence desc.

    Each entry: {"hz": float, "db": float, "prominence": float}
    """
    mono = y.mean(axis=0) if y.ndim == 2 else y

    # Average FFT over the full track — 32k window gives ~1.3 Hz resolution at 44.1 kHz.
    n_fft = 32768
    hop_size = n_fft // 2
    window = np.hanning(n_fft)

    # Keep the per-frame spectra so we can measure how *persistent* each peak is.
    # A room-mode / resonance rings through most of the track regardless of the
    # notes played; a held musical note only shows up in the frames where it
    # sounds. Requiring persistence stops sustained harmonics being flagged.
    frames_db: list[npt.NDArray] = []
    if len(mono) < n_fft:
        padded = np.pad(mono, (0, n_fft - len(mono)))
        spectrum_mag = np.abs(np.fft.rfft(padded * window))
        frames_db.append(20.0 * np.log10(spectrum_mag + 1e-12))
    else:
        n_blocks = 0
        spectrum_mag = np.zeros(n_fft // 2 + 1)
        for i in range(0, len(mono) - n_fft + 1, hop_size):
            frame_mag = np.abs(np.fft.rfft(mono[i : i + n_fft] * window))
            spectrum_mag += frame_mag
            frames_db.append(20.0 * np.log10(frame_mag + 1e-12))
            n_blocks += 1
        if n_blocks > 0:
            spectrum_mag /= n_blocks

    # dBFS (full-scale sine = 0 dB); per-frame dB below is only used
    # relative to its own local floor, so it needs no normalisation.
    spectrum_db = 20.0 * np.log10(spectrum_mag / _amplitude_norm(window) + 1e-12)
    freqs = np.fft.rfftfreq(n_fft, d=1.0 / sr)

    mask = (freqs >= 20) & (freqs <= 18000)
    sub_spec  = spectrum_db[mask]
    sub_freqs = freqs[mask]
    mask_idx  = np.nonzero(mask)[0]

    if len(sub_spec) == 0:
        return {"hotspots": []}

    from scipy import signal as sig  # lazy (PERF-4)

    peaks, props = sig.find_peaks(sub_spec, prominence=8.0)
    if len(peaks) == 0:
        return {"hotspots": []}

    # Persistence: fraction of frames where this bin sits >= 6 dB above the
    # local spectral floor (median of a ±0.5-octave neighbourhood).
    def _persistence(global_bin: int) -> float:
        if len(frames_db) <= 1:
            return 1.0
        lo = max(0, int(global_bin * 0.7))
        hi = min(spectrum_db.shape[0], int(global_bin * 1.4) + 1)
        hits = 0
        for f in frames_db:
            local_floor = float(np.median(f[lo:hi])) if hi > lo else float(np.median(f))
            if f[global_bin] - local_floor >= 6.0:
                hits += 1
        return hits / len(frames_db)

    prominences = props["prominences"]
    candidates = []
    for i in range(len(peaks)):
        persist = _persistence(int(mask_idx[peaks[i]]))
        if persist < 0.5:   # transient/musical peak — not a standing resonance
            continue
        candidates.append((
            float(prominences[i]) * persist,   # rank by prominence weighted by how constant it is
            {
                "hz":          float(sub_freqs[peaks[i]]),
                "db":          float(sub_spec[peaks[i]]),
                "prominence":  float(prominences[i]),
                "persistence": float(persist),
            },
        ))

    candidates.sort(key=lambda c: c[0], reverse=True)
    return {"hotspots": [c[1] for c in candidates[:max_hotspots]]}


def analyze_symmetry(y: np.ndarray, sr: int) -> dict:
    """Measures the symmetry and skewness of the waveform to identify headroom-wasting asymmetry."""
    mono = y.mean(axis=0) if y.ndim == 2 else y
    if len(mono) < 1000:
        return {"skewness": 0.0, "peak_asymmetry_ratio": 1.0, "is_asymmetric": False}

    mean = np.mean(mono)
    std = np.std(mono) + 1e-12
    skew = float(np.mean((mono - mean) ** 3) / (std ** 3))

    max_pos = float(np.max(mono))
    max_neg = float(np.abs(np.min(mono)))

    ratio = max_pos / max_neg if max_neg > 1e-12 else 1.0

    # Sane threshold for asymmetry: ratio > 1.25 or < 0.8 (25% imbalance)
    is_asymmetric = abs(skew) > 0.15 or ratio > 1.25 or ratio < 0.8

    return {
        "skewness": skew,
        "peak_asymmetry_ratio": ratio,
        "is_asymmetric": is_asymmetric
    }


# ---------------------------------------------------------------------------
# Stereo field over time — time-windowed correlation/width/sub-bass-mono%
# ---------------------------------------------------------------------------
def analyze_stereo_field_over_time(y: npt.NDArray, sr: int, n_windows: int = 8) -> dict[str, Any]:
    """Time-windowed companion to analyze_mono_compat(): the whole-track
    per-band correlation there hides *when* a mix loses/gains stereo
    information (e.g. a mono intro widening into the chorus, or bass
    drifting out of mono-compatibility only in the bridge). Runs the same
    correlation math plus the shared mid-side width per window.

    width = clamp(2*S^2 / (M^2 + S^2), 0, 1) with M=(L+R)/2, S=(L-R)/2
    energies (dsp_metrics.stereo_width) — 0 = mono, 1 = uncorrelated
    equal-power L/R, anti-phase clamps to 1. The same definition as the C++
    width meter, so the desktop and the service agree.
    """
    from khensu_services.analysis.time_windows import time_window_bounds

    if y.ndim != 2 or y.shape[0] < 2:
        return {"points": []}

    left, right = y[0], y[1]
    n = len(left)
    bounds = time_window_bounds(n, sr, n_windows)

    points = []
    for i, (start, end) in enumerate(bounds):
        l_seg, r_seg = left[start:end], right[start:end]
        if len(l_seg) < sr * 0.1:
            continue

        seg_stereo = np.stack([l_seg, r_seg])
        mono_result = analyze_mono_compat(seg_stereo, sr)

        width = stereo_width(l_seg, r_seg)
        correlation = stereo_correlation(l_seg, r_seg)

        points.append({
            "window_index": i,
            "progress_fraction": round(start / n, 4),
            "correlation": round(correlation, 3),
            "width": round(width, 3),
            # Was the low-end STEREO index under a "mono" name - the opposite
            # meaning (DSP-14). Now the complement: 100 = mono low end.
            "sub_bass_mono_pct": round(100.0 - mono_result["low_end_stereo_pct"], 2),
        })

    return {"points": points}
