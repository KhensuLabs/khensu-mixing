# local-services/khensu_services/analysis/drum_analyzer.py
"""
Kick and snare detection via band-specific onset detection.

Approach:
  Kick:  onset detection on the 50–120 Hz band (sub-bass transients).
  Snare: onset detection on the 150–8000 Hz band, de-duplicated against kick onsets.

Each detected event is characterised by:
  - onset time (seconds)
  - attack time: samples from onset frame to peak in the 50 ms window
  - body level dB: RMS in the 10–40 ms window after onset (sustain phase)
  - fundamental Hz: dominant FFT bin in a 50 ms window around the onset

Aggregate statistics (count, average BPM, timing regularity as 1-CV) are
returned for use by the recommendation engine and the UI.
"""
from __future__ import annotations
import numpy as np
import numpy.typing as npt
from scipy import signal as sig
from typing import Any


# ---------------------------------------------------------------------------
# Internal helpers
# ---------------------------------------------------------------------------

def _mono(y: npt.NDArray) -> npt.NDArray:
    return y.mean(axis=0) if y.ndim == 2 else y


def _bandpass(y: npt.NDArray, sr: int, low_hz: float, high_hz: float) -> npt.NDArray:
    mono = _mono(y)
    nyq  = sr / 2.0
    lo   = max(low_hz  / nyq, 1e-4)
    hi   = min(high_hz / nyq, 0.9999)
    if lo >= hi:
        return mono
    b_arr: npt.NDArray
    a_arr: npt.NDArray
    b_arr, a_arr = sig.butter(4, [lo, hi], btype="band")  # type: ignore[misc]
    return sig.filtfilt(b_arr, a_arr, mono).astype(np.float32)


def _band_onset_times(y_perc: npt.NDArray, sr: int,
                      band_lo: float, band_hi: float,
                      hop_length: int = 512,
                      min_interval_sec: float = 0.10) -> npt.NDArray:
    """Onset times from band-limited spectral flux of the percussive signal.

    Replaces the old mel-spectrogram onset_strength approach, which failed on
    full mixes in both directions: sustained bass/pads in the band masked real
    hits, and — because librosa normalizes the onset envelope — steady signals
    with NO transients at all produced dozens of false onsets from numerical
    noise. Two properties fix that here:

      1. Flux is computed only from the target band of the *percussive* HPSS
         component, so sustained tonal energy (808 bassline under the kick,
         pads/vocals under the snare) never reaches the detector.
      2. Peaks must clear an absolute contrast gate: the strongest flux in
         the band must itself be significant relative to the band's sustained
         level, and each peak must be a large multiple of the flux noise
         floor (median + MAD). A drumless band therefore yields zero events
         instead of normalized-noise peaks.
    """
    import librosa

    n_fft = 2048
    S = np.abs(librosa.stft(y_perc, n_fft=n_fft, hop_length=hop_length))
    freqs = librosa.fft_frequencies(sr=sr, n_fft=n_fft)
    band = S[(freqs >= band_lo) & (freqs <= band_hi)]
    if band.size == 0 or band.shape[1] < 4:
        return np.array([])

    env = band.sum(axis=0)
    flux = np.maximum(0.0, np.diff(env, prepend=env[:1]))

    med_env = float(np.median(env))
    peak_flux = float(np.max(flux))
    # No-transient gate: if the biggest energy jump in this band is small
    # relative to the band's sustained level, there is nothing drum-like here.
    if peak_flux < max(0.5 * med_env, 1e-6):
        return np.array([])

    med_flux = float(np.median(flux))
    mad_flux = float(np.median(np.abs(flux - med_flux))) + 1e-12
    wait = max(1, int(min_interval_sec * sr / hop_length))
    peaks = librosa.util.peak_pick(
        flux, pre_max=3, post_max=3, pre_avg=8, post_avg=8,
        delta=med_flux + 8.0 * mad_flux, wait=wait,
    )
    # Per-peak contrast gate: a real hit is a substantial fraction of the
    # strongest hit in the band; spillover ripple is not. Peaks inside the
    # first/last pre_avg frames are dropped too — flux there has no valid
    # baseline, so signal turn-on and end-of-file STFT padding both read as
    # huge jumps.
    n_frames = flux.shape[0]
    peaks = np.asarray([p for p in np.atleast_1d(peaks)
                        if flux[p] >= 0.15 * peak_flux
                        and 8 <= p < n_frames - 8], dtype=int)
    if peaks.size == 0:
        return np.array([])

    times = librosa.frames_to_time(peaks, sr=sr, hop_length=hop_length)

    deduped = [float(times[0])]
    for t in times[1:]:
        if t - deduped[-1] >= min_interval_sec:
            deduped.append(float(t))
    return np.array(deduped)


def _characterise(onsets: npt.NDArray, y_mono: npt.NDArray, sr: int,
                  band_lo: float, band_hi: float) -> list[dict[str, Any]]:
    """Per-onset characterisation: attack, body level, fundamental."""
    window_samples = int(sr * 0.05)   # 50 ms analysis window
    body_start     = int(sr * 0.010)  # 10 ms after onset
    body_end       = int(sr * 0.040)  # 40 ms after onset
    events: list[dict[str, Any]] = []

    for t in onsets:
        i0 = int(t * sr)
        win = y_mono[i0 : i0 + window_samples]
        if len(win) < 64:
            continue

        # Attack time: samples to peak within the window
        peak_idx   = int(np.argmax(np.abs(win)))
        attack_ms  = float(peak_idx / sr * 1000.0)

        # Body level: RMS in 10–40 ms after onset
        body = win[body_start : body_end]
        body_rms   = float(np.sqrt(np.mean(body ** 2) + 1e-12))
        body_db    = float(20.0 * np.log10(body_rms))

        # Dominant frequency: largest FFT bin in the window
        fft    = np.abs(np.fft.rfft(win * np.hanning(len(win))))
        freqs  = np.fft.rfftfreq(len(win), d=1.0 / sr)
        mask   = (freqs >= band_lo) & (freqs <= band_hi)
        if mask.any():
            fund_hz = float(freqs[mask][int(np.argmax(fft[mask]))])
        else:
            fund_hz = 0.0

        events.append({
            "onset_sec": round(float(t), 4),
            "attack_ms": round(attack_ms, 2),
            "body_db":   round(body_db, 2),
            "fund_hz":   round(fund_hz, 1),
        })

    return events


def _aggregate(events: list[dict[str, Any]]) -> dict[str, Any]:
    """Compute count, avg_bpm, timing_regularity, and mean characterisation.

    ``avg_bpm`` is the hit RATE in hits per minute (60 / mean inter-onset
    interval), not the track tempo: kicks on beats 1 and 3 at 120 BPM read
    60 (THY-11). The key name is kept for wire compatibility; the UI shows it
    as "kicks/min"."""
    count = len(events)
    if count == 0:
        return {
            "count": 0, "avg_bpm": 0.0, "timing_regularity": 0.0,
            "avg_body_db": -60.0, "avg_fund_hz": 0.0,
        }

    onsets = np.array([e["onset_sec"] for e in events])
    ibi    = np.diff(onsets)

    if len(ibi) >= 2:
        cv            = float(np.std(ibi) / (np.mean(ibi) + 1e-9))
        regularity    = float(np.clip(1.0 - cv, 0.0, 1.0))
        avg_bpm       = float(60.0 / (np.mean(ibi) + 1e-9))
    elif len(ibi) == 1:
        regularity = 1.0
        avg_bpm    = float(60.0 / (ibi[0] + 1e-9))
    else:
        regularity = 1.0
        avg_bpm    = 0.0

    avg_body_db = float(np.mean([e["body_db"]  for e in events]))
    avg_fund_hz = float(np.mean([e["fund_hz"]  for e in events if e["fund_hz"] > 0]))

    return {
        "count":             count,
        "avg_bpm":           round(avg_bpm, 1),
        "timing_regularity": round(regularity, 3),
        "avg_body_db":       round(avg_body_db, 1),
        "avg_fund_hz":       round(avg_fund_hz, 1),
    }


# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------

def analyze_drums(y: npt.NDArray, sr: int) -> dict[str, Any]:
    """Detect and characterise kick and snare events.

    Returns:
        kick:   aggregate kick stats + event list
        snare:  aggregate snare stats + event list
        transient_sharpness: 0–1, ratio of fast attacks (< 10 ms) across all events
    """

    y_mono = _mono(y)

    # HPSS percussive isolation: sustained tonal content (808 basslines under
    # the kick band, pads/vocals across the snare band) is what defeated the
    # old full-mix detection; only the percussive residue reaches the onset
    # detector. margin=2.0 keeps soft/808-style attacks that a stricter
    # margin would discard along with the harmonics.
    # Kernel-clamped HPSS: librosa's own went NaN on clips under ~0.7 s and
    # the onset STFT below then raised (see _shared_analysis.hpss).
    from khensu_services.analysis._shared_analysis import hpss_signal
    try:
        _, y_perc = hpss_signal(y_mono.astype(np.float32), margin=2.0)
    except Exception:
        y_perc = y_mono

    # Band-limited spectral-flux onset detection on the percussive signal.
    # Kick band reaches down to 35 Hz for 808/trap kicks whose fundamental
    # sits below the old 50 Hz floor. min_interval enforces a physical
    # minimum gap: kicks can't repeat faster than ~240 BPM (250 ms), so
    # 150 ms prevents split-onset artefacts.
    kick_times  = _band_onset_times(y_perc, sr, 35.0,  150.0, min_interval_sec=0.15)
    snare_times = _band_onset_times(y_perc, sr, 150.0, 8000.0, min_interval_sec=0.10)

    # Remove snare detections that are within 30 ms of a kick onset
    # (the kick's broadband transient can bleed into the snare band)
    if len(kick_times) > 0 and len(snare_times) > 0:
        def _not_near_kick(t: float) -> bool:
            return bool(np.all(np.abs(kick_times - t) > 0.030))
        snare_times = np.array([t for t in snare_times if _not_near_kick(t)])

    # Characterise events
    kick_events  = _characterise(kick_times,  y_mono, sr, 50.0,  120.0)
    snare_events = _characterise(snare_times, y_mono, sr, 150.0, 8000.0)

    # Transient sharpness: fraction of all events with attack < 10 ms
    all_attacks = [e["attack_ms"] for e in kick_events + snare_events]
    if all_attacks:
        sharpness = float(np.mean([1.0 if a < 10.0 else 0.0 for a in all_attacks]))
    else:
        sharpness = 0.0

    kick_agg  = _aggregate(kick_events)
    snare_agg = _aggregate(snare_events)

    # Kick/bass sub proximity check — detect when the kick fundamental and the
    # dominant sub-bass frequency are within ±10 Hz, which causes frequency
    # masking and muddy low end.
    proximity = _check_kick_bass_proximity(y_mono, sr, kick_agg.get("avg_fund_hz", 0.0))

    return {
        "kick":  {**kick_agg,  "events": kick_events[:20]},
        "snare": {**snare_agg, "events": snare_events[:20]},
        "transient_sharpness":       round(sharpness, 3),
        "kick_bass_proximity_hz":    proximity["proximity_hz"],
        "kick_bass_frequency_clash": proximity["clash"],
        "bass_fundamental_hz":       proximity["bass_fundamental_hz"],
    }


def _check_kick_bass_proximity(y_mono: npt.NDArray, sr: int,
                                kick_fund_hz: float) -> dict[str, Any]:
    """Detect frequency proximity between kick fundamental and bass sub.

    Extracts the dominant frequency in the 40–120 Hz sub-bass region (sustained
    energy, not transient) and compares to the kick fundamental. If they are
    within ±10 Hz the low end will exhibit masking and phase beating.

    Returns:
        bass_fundamental_hz: detected dominant bass frequency
        proximity_hz: absolute difference between kick fund and bass fund
        clash: True when proximity_hz < 10 Hz and both frequencies > 0
    """
    if kick_fund_hz <= 0.0 or len(y_mono) < sr // 2:
        return {"bass_fundamental_hz": 0.0, "proximity_hz": 0.0, "clash": False}

    # Use the sustained (non-transient) signal for bass: high-percentile RMS windows
    # removed so only the sustained tone contributes, not the kick attack.
    # 400 ms windows give 2.5 Hz FFT bins — a 100 ms window's 10 Hz bins made
    # the "<10 Hz clash" test equal to its own resolution (a coin flip).
    win   = min(int(sr * 0.4), len(y_mono))
    hop   = win // 2
    rms_wins = [(j, float(np.sqrt(np.mean(y_mono[j:j+win]**2))))
                for j in range(0, len(y_mono) - win, hop)]
    if not rms_wins:
        return {"bass_fundamental_hz": 0.0, "proximity_hz": 0.0, "clash": False}

    # Use windows above the 75th-pct RMS (sustained/body, not transient)
    rms_vals = [r for _, r in rms_wins]
    p75 = float(np.percentile(rms_vals, 75))
    sustained_idxs = [j for j, r in rms_wins if r >= p75 * 0.5]

    if not sustained_idxs:
        return {"bass_fundamental_hz": 0.0, "proximity_hz": 0.0, "clash": False}

    # Accumulate FFT of sustained windows in the 40–120 Hz range
    accumulated = np.zeros(win // 2 + 1)
    for j in sustained_idxs[:32]:  # cap at 32 windows to bound compute
        seg = y_mono[j:j+win] * np.hanning(win)
        accumulated += np.abs(np.fft.rfft(seg))

    freqs = np.fft.rfftfreq(win, d=1.0 / sr)
    sub_mask = (freqs >= 40.0) & (freqs <= 120.0)
    if not sub_mask.any():
        return {"bass_fundamental_hz": 0.0, "proximity_hz": 0.0, "clash": False}

    sub_spec  = accumulated[sub_mask]
    sub_freqs = freqs[sub_mask]
    bass_fund = float(sub_freqs[int(np.argmax(sub_spec))])

    proximity_hz = float(abs(kick_fund_hz - bass_fund))
    clash = proximity_hz < 10.0 and kick_fund_hz > 0.0 and bass_fund > 0.0

    return {
        "bass_fundamental_hz": round(bass_fund, 1),
        "proximity_hz":        round(proximity_hz, 1),
        "clash":               clash,
    }
