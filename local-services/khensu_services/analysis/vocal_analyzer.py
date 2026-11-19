"""Vocal-specific time-resolved analysis.

Produces data the Vocal Dashboard needs but that the single-scalar
extended-analysis functions don't provide:

  - sibilance_timeline: per-window sibilance (5-10 kHz vs 1-10 kHz) so the UI
    can draw a heatmap instead of a single number.
  - breath_markers: low-energy, high-HF-noise, low-pitch-confidence moments
    between phrases — candidate breath / mouth-noise positions on the waveform.
  - presence/clarity/warmth: vocal balance tiles.
  - formant_balance: energy in the vocal formant regions (F1 ~500, F2 ~1500,
    F3 ~2500 Hz zones) distinct from a generic octave spectrum.

Runs on the mix or, better, an isolated vocal stem when one is available.
"""
from __future__ import annotations

import numpy as np
import numpy.typing as npt
import os
from khensu_services.analysis.context import load_audio


def _band_energy(mag: np.ndarray, freqs: np.ndarray, lo: float, hi: float) -> np.ndarray:
    m = (freqs >= lo) & (freqs < hi)
    return np.sum(mag[m, :] ** 2, axis=0)


def analyze_vocal(y: npt.NDArray, sr: int, vocals_path: str = None) -> dict:
    """Time-resolved vocal metrics. `y` may be (channels, samples) or mono."""
    mono = y.mean(axis=0) if y.ndim == 2 else y
    n = len(mono)
    if n < sr // 2:
        return {
            "sibilance_timeline": [], "breath_markers": [], "plosive_markers": [],
            "presence_score": 0.0, "clarity_score": 0.0, "warmth_score": 0.0,
            "formant_balance": {}, "window_sec": 0.0, "dominant_sibilance_hz": 0.0,
            "sii_timeline": [], "vocal_sii_score": 1.0,
        }

    n_fft = 2048
    hop = 512
    win = np.hanning(n_fft)
    freqs = np.fft.rfftfreq(n_fft, d=1.0 / sr)

    # STFT magnitude (freq x frames)
    frames = range(0, n - n_fft + 1, hop)
    mag = np.empty((len(freqs), 0))
    cols = []
    for start in frames:
        seg = mono[start:start + n_fft] * win
        cols.append(np.abs(np.fft.rfft(seg)))
    mag = np.stack(cols, axis=1) if cols else np.zeros((len(freqs), 1))
    times = np.arange(mag.shape[1]) * hop / sr
    window_sec = hop / sr

    total = np.sum(mag ** 2, axis=0) + 1e-12
    e_presence = _band_energy(mag, freqs, 1000.0, 5000.0)
    e_mud = _band_energy(mag, freqs, 200.0, 1000.0) + 1e-12
    e_warmth = _band_energy(mag, freqs, 120.0, 400.0)
    e_plosive = _band_energy(mag, freqs, 40.0, min(220.0, sr * 0.45))
    e_vocal_mid = _band_energy(mag, freqs, 250.0, min(3000.0, sr * 0.45))
    e_sib_hi = _band_energy(mag, freqs, 5000.0, min(10000.0, sr * 0.45))
    e_sib_ref = _band_energy(mag, freqs, 1000.0, min(10000.0, sr * 0.45)) + 1e-12
    e_hf_noise = _band_energy(mag, freqs, 8000.0, min(16000.0, sr * 0.45))

    rms = np.sqrt(total / mag.shape[0])
    rms_db = 20.0 * np.log10(rms + 1e-9)

    # --- Sibilance timeline (0..1 per window) ---
    sib = e_sib_hi / e_sib_ref
    sib_norm = np.clip(sib / 0.6, 0.0, 1.0)  # 0.6 ratio ~ strongly sibilant
    # Gate sibilance by absolute level to prevent false triggers in silent/quiet gaps
    sib_norm = np.where(rms_db > -60.0, sib_norm, 0.0)

    # --- Dominant sibilance frequency ---
    sib_band_mask = (freqs >= 5000.0) & (freqs < min(10000.0, sr * 0.45))
    sib_gate = max(0.15, float(np.percentile(sib_norm, 90))) if sib_norm.size else 1.0
    sibilant_frames = sib_norm >= sib_gate
    if sib_band_mask.any() and np.any(sibilant_frames):
        sib_band_freqs = freqs[sib_band_mask]
        sib_band_power = mag[sib_band_mask, :][:, sibilant_frames] ** 2
        weight_sum = np.sum(sib_band_power)
        dominant_sibilance_hz = (
            float(np.sum(sib_band_freqs[:, np.newaxis] * sib_band_power) / weight_sum)
            if weight_sum > 1e-12 else 0.0
        )
    else:
        dominant_sibilance_hz = 0.0

    # Downsample to ~10 Hz for a compact heatmap.
    stride = max(1, int(0.1 / window_sec))
    timeline = [
        {"time_sec": float(times[i]), "sibilance": float(sib_norm[i])}
        for i in range(0, len(sib_norm), stride)
    ]

    # --- Breath / mouth-noise markers ---
    gate = np.percentile(rms_db, 35)          # quiet frames
    hf_share = e_hf_noise / total
    hf_gate = np.percentile(hf_share, 70)     # relatively bright for their level

    markers = []
    in_marker = False
    m_start = 0.0
    for i in range(len(rms_db)):
        is_breath = rms_db[i] < gate and hf_share[i] > hf_gate
        if is_breath and not in_marker:
            in_marker = True
            m_start = float(times[i])
        elif not is_breath and in_marker:
            in_marker = False
            dur = float(times[i]) - m_start
            if dur >= 0.06:  # ignore sub-frame flickers
                markers.append({"time_sec": round(m_start, 3),
                                "duration_sec": round(dur, 3)})
    if in_marker:
        markers.append({"time_sec": round(m_start, 3),
                        "duration_sec": round(float(times[-1]) - m_start, 3)})

    # --- Low-frequency plosive candidates ---
    # Plosives are brief low-band bursts at an onset.  A sustained bassy vowel
    # can also be low-heavy, so it is deliberately not flagged unless the
    # low/mid energy rises rapidly.  This is a candidate detector for manual
    # review, not a phoneme classifier.
    plosive_ratio = e_plosive / (e_plosive + e_vocal_mid + 1e-12)
    plosive_db = 10.0 * np.log10(e_plosive + 1e-12)
    prior_low_db = np.concatenate(([plosive_db[0]], plosive_db[:-1]))
    low_attack_db = np.maximum(0.0, plosive_db - prior_low_db)
    plosive_active = (
        (rms_db > -55.0)
        & (plosive_ratio >= 0.55)
        & (low_attack_db >= 2.5)
    )
    plosive_markers = []
    in_plosive = False
    plosive_start = 0.0
    marker_peak_ratio = 0.0
    marker_peak_attack = 0.0
    for i, active in enumerate(plosive_active):
        if active and not in_plosive:
            in_plosive = True
            plosive_start = float(times[i])
            marker_peak_ratio = float(plosive_ratio[i])
            marker_peak_attack = float(low_attack_db[i])
        elif active:
            marker_peak_ratio = max(marker_peak_ratio, float(plosive_ratio[i]))
            marker_peak_attack = max(marker_peak_attack, float(low_attack_db[i]))
        elif in_plosive:
            in_plosive = False
            duration = max(window_sec, float(times[i]) - plosive_start)
            severity = float(np.clip(
                0.65 * (marker_peak_ratio - 0.55) / 0.45
                + 0.35 * (marker_peak_attack - 2.5) / 12.0,
                0.0, 1.0,
            ))
            plosive_markers.append({"time_sec": round(plosive_start, 3),
                                    "duration_sec": round(duration, 3),
                                    "severity": severity,
                                    "low_band_ratio": float(marker_peak_ratio)})
    if in_plosive:
        duration = max(window_sec, float(times[-1]) - plosive_start + window_sec)
        severity = float(np.clip(
            0.65 * (marker_peak_ratio - 0.55) / 0.45
            + 0.35 * (marker_peak_attack - 2.5) / 12.0,
            0.0, 1.0,
        ))
        plosive_markers.append({"time_sec": round(plosive_start, 3),
                                "duration_sec": round(duration, 3),
                                "severity": severity,
                                "low_band_ratio": float(marker_peak_ratio)})

    # --- Balance tiles (whole-clip means) ---
    presence_score = float(np.clip(np.mean(e_presence / total) / 0.25, 0.0, 1.0))
    clarity_score = float(np.clip(np.mean(e_presence / e_mud) / 2.0, 0.0, 1.0))
    warmth_score = float(np.clip(np.mean(e_warmth / total) / 0.20, 0.0, 1.0))

    # --- Formant balance (F1/F2/F3 zones) ---
    def frac(lo, hi):
        return float(np.mean(_band_energy(mag, freqs, lo, hi) / total))
    formant_balance = {
        "f1_300_800":   frac(300.0, 800.0),     # openness / body
        "f2_800_1800":  frac(800.0, 1800.0),    # vowel intelligibility
        "f3_1800_3000": frac(1800.0, 3000.0),   # presence / projection
        "air_8k_plus":  frac(8000.0, min(16000.0, sr * 0.45)),
    }

    # --- Speech Intelligibility Index (SII) calculation ---
    sii_timeline = []
    vocal_sii_score = 1.0
    has_backing = False
    y_backing = None

    if vocals_path and os.path.basename(vocals_path) == "vocals.wav":
        parent_dir = os.path.dirname(vocals_path)
        backing_sigs = []
        for stem_name in ["drums.wav", "bass.wav", "other.wav"]:
            sibling = os.path.join(parent_dir, stem_name)
            if os.path.exists(sibling):
                try:
                    yb, _ = load_audio(sibling, sr=sr, mono=True)
                    backing_sigs.append(yb)
                except Exception:
                    pass
        if backing_sigs:
            min_len = len(mono)
            y_backing = np.zeros(min_len)
            for yb in backing_sigs:
                L = min(min_len, len(yb))
                y_backing[:L] += yb[:L]
            has_backing = True

    if has_backing:
        cols_b = []
        for start in frames:
            seg_b = y_backing[start:start + n_fft] * win
            cols_b.append(np.abs(np.fft.rfft(seg_b)))
        mag_b = np.stack(cols_b, axis=1) if cols_b else np.zeros((len(freqs), 1))

        # Standard Speech Intelligibility critical band parameters mapped to vocal frequencies
        # band: (low_hz, high_hz, importance_weight)
        sii_bands = [
            (120.0, 250.0, 0.05),
            (250.0, 500.0, 0.15),
            (500.0, 1000.0, 0.25),
            (1000.0, 2000.0, 0.30),
            (2000.0, 4000.0, 0.20),
            (4000.0, 8000.0, 0.05)
        ]

        frame_siis = []
        for f_idx in range(mag.shape[1]):
            # If vocals are below gate, they are silent; masking risk is not active
            if rms_db[f_idx] < -50.0:
                frame_siis.append(1.0)
                continue

            sii_sum = 0.0
            weight_norm = 0.0
            for lo, hi, weight in sii_bands:
                mask = (freqs >= lo) & (freqs < hi)
                if not np.any(mask):
                    continue

                e_v = np.sum(mag[mask, f_idx] ** 2)
                e_b = np.sum(mag_b[mask, f_idx] ** 2)

                db_v = 10.0 * np.log10(e_v + 1e-12)
                db_b = 10.0 * np.log10(e_b + 1e-12)

                snr = db_v - db_b
                snr_clamped = max(-15.0, min(15.0, snr))
                audibility = (snr_clamped + 15.0) / 30.0

                sii_sum += weight * audibility
                weight_norm += weight

            sii_val = sii_sum / weight_norm if weight_norm > 0 else 1.0
            frame_siis.append(sii_val)

        frame_siis = np.array(frame_siis)
        active_frames = rms_db > -50.0
        if np.any(active_frames):
            vocal_sii_score = float(np.mean(frame_siis[active_frames]))
        else:
            vocal_sii_score = 1.0

        sii_timeline = [
            {"time_sec": float(times[i]), "sii": float(frame_siis[i])}
            for i in range(0, len(frame_siis), stride)
        ]

    return {
        "sibilance_timeline": timeline,
        "breath_markers": markers[:100],
        "plosive_markers": plosive_markers[:100],
        "presence_score": presence_score,
        "clarity_score": clarity_score,
        "warmth_score": warmth_score,
        "formant_balance": formant_balance,
        "window_sec": float(window_sec * stride),
        "dominant_sibilance_hz": dominant_sibilance_hz,
        "sii_timeline": sii_timeline,
        "vocal_sii_score": vocal_sii_score,
    }
