"""Per-section audio analysis and section boundary detection."""
from __future__ import annotations
import numpy as np
from khensu_services.analysis import _lean_spectral as _lean
import numpy.typing as npt
from typing import Any
from khensu_services.analysis.dsp_metrics import onset_strength

_MIN_SECTION_DURATION_SEC = 4.0
_BARS_PER_SECTION = 8
# Windows quieter than -120 dBFS RMS are digital silence, not a noise floor.
_NOISE_FLOOR_GATE_LIN = 10.0 ** (-120.0 / 20.0)


def detect_sections(y: npt.NDArray, sr: int) -> list[dict[str, Any]]:
    """
    Detect section boundaries using librosa beat tracking: 8-bar blocks on
    the estimated meter, aligned to the estimated downbeat phase.

    Returns sections with start_sec, end_sec, duration_sec, label.
    Sections shorter than _MIN_SECTION_DURATION_SEC are merged with neighbours.
    Labels are sequential: section_1, section_2, ...
    """
    import librosa

    # Both channels: reading only the first made a track's structure depend
    # on which side the music was on (a dead left channel gave one section).
    mono = y.mean(axis=0) if y.ndim == 2 else y
    total_duration = float(len(mono) / sr)

    # NEW-1: real structure first; the 8-bar grid below only when the track
    # has too few bars to segment.
    try:
        from khensu_services.analysis.song_structure import analyze_structure
        found = analyze_structure(mono, sr)
        if found["method"] == "self_similarity" and len(found["sections"]) >= 2:
            return [{"start_sec": s["start_sec"], "end_sec": s["end_sec"],
                     "duration_sec": s["duration_sec"], "label": s["label"], "role": s["role"],
                     "group": s["group"]} for s in found["sections"]]
    except Exception:
        pass

    try:
        from khensu_services.analysis._shared_analysis import estimate_meter, tracked_tempo

        tempo, beat_frames = _lean.beat_track(y=mono, sr=sr)
        beat_times = librosa.frames_to_time(beat_frames, sr=sr).tolist()
        # 8-bar sections on the detected meter, starting at the first
        # downbeat - not 4/4 counted from whatever beat the tracker found
        # first (a pickup or a waltz used to shift every boundary).
        meter = estimate_meter(mono, sr, beat_times, tracked_tempo(tempo, beat_times))
        beats_per_section = _BARS_PER_SECTION * max(1, int(meter["beats_per_bar"]))
        phase = int(meter["downbeat_phase"])

        if len(beat_times) >= beats_per_section:
            # A pickup before the first bar line folds into the first section.
            starts = [float(beat_times[i])
                      for i in range(phase, len(beat_times), beats_per_section)]
        else:
            starts = [0.0]
    except Exception:
        starts = [0.0]

    if not starts or starts[0] > 0.0:
        starts.insert(0, 0.0)

    raw: list[dict[str, Any]] = []
    for i, start in enumerate(starts):
        end = starts[i + 1] if i + 1 < len(starts) else total_duration
        raw.append({
            "start_sec":    float(start),
            "end_sec":      float(end),
            "duration_sec": float(end - start),
        })

    merged = _merge_short_sections(raw, total_duration)
    for idx, section in enumerate(merged):
        section["label"] = f"section_{idx + 1}"

    return merged


def _merge_short_sections(
    sections: list[dict[str, Any]],
    total_duration: float,
) -> list[dict[str, Any]]:
    """Merge sections shorter than _MIN_SECTION_DURATION_SEC into neighbours."""
    if not sections:
        return [{"start_sec": 0.0, "end_sec": total_duration, "duration_sec": total_duration}]

    result: list[dict[str, Any]] = [dict(s) for s in sections]
    changed = True
    while changed:
        changed = False
        i = 0
        while i < len(result):
            if result[i]["duration_sec"] < _MIN_SECTION_DURATION_SEC and len(result) > 1:
                if i + 1 < len(result):
                    # Extend next section backwards
                    result[i + 1]["start_sec"]    = result[i]["start_sec"]
                    result[i + 1]["duration_sec"]  = (
                        result[i + 1]["end_sec"] - result[i]["start_sec"]
                    )
                    result.pop(i)
                else:
                    # Extend previous section forwards
                    result[i - 1]["end_sec"]      = result[i]["end_sec"]
                    result[i - 1]["duration_sec"]  = (
                        result[i - 1]["end_sec"] - result[i - 1]["start_sec"]
                    )
                    result.pop(i)
                changed = True
            else:
                i += 1

    return result


def analyze_sections(
    y: npt.NDArray,
    sr: int,
    sections: list[dict],
    total_duration_sec: float,
) -> list[dict[str, Any]]:
    """
    Analyze each section of a track independently.

    Accepts sections as ``[{"start_sec": float, "label": str}, ...]`` (as returned
    by quick_analyzer) and computes per-section audio metrics.

    Sections shorter than _MIN_SECTION_DURATION_SEC get partial results
    (lufs=None, crest_db=None, etc.) to avoid misleading measurements.
    """
    import librosa

    # Pre-compute onset-strength envelope once for boundary confidence.
    # Boundary confidence = onset flux at the detected cut time / (3 × mean flux).
    # The first section always starts at 0 (confidence = 1.0).
    _hop = 512
    mono_full = y.mean(axis=0) if y.ndim == 2 else y
    try:
        _onset_env   = onset_strength(mono_full, sr, hop_length=_hop)
        _onset_times = librosa.times_like(_onset_env, sr=sr, hop_length=_hop)
        _mean_flux   = float(np.mean(_onset_env) + 1e-9)
    except Exception:
        _onset_env   = None
        _onset_times = None
        _mean_flux   = 1.0

    results: list[dict[str, Any]] = []

    for i, section in enumerate(sections):
        start_sec = float(section.get("start_sec", 0.0))
        label     = str(section.get("label", f"S{i}"))

        end_sec = (
            float(sections[i + 1]["start_sec"])
            if i + 1 < len(sections)
            else total_duration_sec
        )
        duration_sec = end_sec - start_sec

        start_sample = int(start_sec * sr)
        end_sample   = int(end_sec   * sr)
        segment = y[:, start_sample:end_sample] if y.ndim == 2 else y[start_sample:end_sample]

        # Boundary confidence from onset flux at this section's start time.
        if _onset_env is not None and _onset_times is not None:
            if i == 0 or start_sec < 0.01:
                _bc: float | None = 1.0
            else:
                idx = int(np.argmin(np.abs(_onset_times - start_sec)))
                _bc = float(np.clip(float(_onset_env[idx]) / _mean_flux / 3.0, 0.0, 1.0))
        else:
            _bc = None

        entry: dict[str, Any] = {
            "label":               label,
            **({"role": section["role"]} if section.get("role") else {}),
            **({"group": section["group"]} if section.get("group") else {}),
            "start_sec":           start_sec,
            "duration_sec":        duration_sec,
            "lufs":                None,
            "crest_db":            None,
            "lra":                 None,
            "stereo_correlation":  None,
            "balance_low":         None,
            "dominant_freq_hz":    None,
            "harshness_score":     None,
            "noise_floor_db":      None,
            "peak_dbfs":           None,
            "sibilance_score":     None,
            "boundary_confidence": _bc,
        }

        if duration_sec < _MIN_SECTION_DURATION_SEC:
            results.append(entry)
            continue

        mono = segment.mean(axis=0) if segment.ndim == 2 else segment
        if len(mono) == 0:
            results.append(entry)
            continue

        # --- LUFS (shared ITU-R BS.1770 meter) ---
        # Measured on the real channel layout (stereo per-channel sum; mono
        # as ONE channel) — a mono downmix reads up to ~3 LU low on wide
        # material. Silence / too-short / failed -> None: an RMS dBFS
        # fallback is not LUFS and must not be labelled as such.
        from khensu_services.analysis.loudness import integrated_lufs, loudness_range
        try:
            entry["lufs"] = integrated_lufs(segment, sr)
        except Exception:
            entry["lufs"] = None

        # --- Crest factor + sample peak ---
        from khensu_services.analysis.loudness import crest_factor_db
        peak = float(np.max(np.abs(mono)) + 1e-12)
        # Per channel, max (see loudness.crest_factor_db); None below -90 dBFS RMS.
        entry["crest_db"]  = crest_factor_db(segment)
        entry["peak_dbfs"] = float(20.0 * np.log10(peak))

        # --- LRA (EBU Tech 3342, shared implementation) ---
        try:
            entry["lra"] = loudness_range(segment, sr)
        except Exception:
            entry["lra"] = None

        # --- Stereo correlation ---
        if segment.ndim == 2 and segment.shape[0] >= 2:
            from khensu_services.analysis.dsp_metrics import stereo_correlation
            entry["stereo_correlation"] = stereo_correlation(segment[0], segment[1])

        # --- Low-end balance (energy below 250 Hz / total) ---
        fft_mag = np.abs(np.fft.rfft(mono))
        freqs   = np.fft.rfftfreq(len(mono), d=1.0 / sr)
        energy_total = float(np.sum(fft_mag ** 2) + 1e-12)
        energy_low   = float(np.sum(fft_mag[freqs < 250] ** 2))
        entry["balance_low"] = energy_low / energy_total

        # --- Dominant frequency ---
        idx_peak = int(np.argmax(fft_mag[1:]) + 1)
        entry["dominant_freq_hz"] = float(freqs[idx_peak])

        # --- Per-section harshness (2.5–5 kHz vs. 1–2.5 kHz warmth ratio) ---
        mask_warm  = (freqs >= 1000.0) & (freqs < 2500.0)
        mask_harsh = (freqs >= 2500.0) & (freqs < 5000.0)
        energy_warm  = float(np.sum(fft_mag[mask_warm]  ** 2) + 1e-12)
        energy_harsh = float(np.sum(fft_mag[mask_harsh] ** 2) + 1e-12)
        harsh_ratio  = energy_harsh / energy_warm
        entry["harshness_score"] = float(np.clip((harsh_ratio - 0.5) / 3.0, 0.0, 1.0))

        # --- Per-section sibilance (5–10 kHz vs. total 1–10 kHz energy) ---
        mask_sib    = (freqs >= 5000.0) & (freqs < 10000.0)
        mask_mid_hi = (freqs >= 1000.0) & (freqs < 10000.0)
        energy_sib    = float(np.sum(fft_mag[mask_sib]    ** 2) + 1e-12)
        energy_mid_hi = float(np.sum(fft_mag[mask_mid_hi] ** 2) + 1e-12)
        # Threshold: 0.10 = low sibilance; 0.50 = very sibilant (score clips at 1.0)
        entry["sibilance_score"] = float(np.clip((energy_sib / energy_mid_hi - 0.10) / 0.40, 0.0, 1.0))

        # --- Noise floor estimate (10th-pct RMS of 50 ms windows, in dBFS) ---
        # Digital-silence windows (below -120 dBFS — e.g. zeroed gaps between
        # takes) are gated out: they'd otherwise pin the "noise floor" at the
        # -240 dB epsilon. All-silent sections report None.
        win50 = max(1, sr // 20)
        rms_50ms = np.array([
            float(np.sqrt(np.mean(mono[j : j + win50] ** 2)))
            for j in range(0, len(mono) - win50, win50)
        ])
        live = rms_50ms[rms_50ms > _NOISE_FLOOR_GATE_LIN] if rms_50ms.size else rms_50ms
        if live.size:
            p10_rms = float(np.percentile(live, 10))
            entry["noise_floor_db"] = float(20.0 * np.log10(p10_rms))
        else:
            entry["noise_floor_db"] = None

        results.append(entry)

    return results


def compute_loudness_arc(sections: list[dict]) -> dict:
    """Compute loudness arc metrics from a list of analysed sections.

    Returns:
        chorus_verse_delta_lu: LU difference between the loudest and quietest
            sections (proxy for chorus-vs-verse energy management).
        max_section_lufs: loudest section LUFS value.
        min_section_lufs: quietest section LUFS value.
        section_count: number of sections with valid LUFS readings.
    """
    # Finite readings only: a silent section must not inject -inf (and the
    # old RMS fallback could inject non-LUFS numbers); reject clip/zero too.
    lufs_values = [
        float(s["lufs"]) for s in sections
        if s.get("lufs") is not None and np.isfinite(s["lufs"]) and s["lufs"] < 0.0
    ]
    if len(lufs_values) < 2:
        return {
            "chorus_verse_delta_lu": None,
            "max_section_lufs": lufs_values[0] if lufs_values else None,
            "min_section_lufs": lufs_values[0] if lufs_values else None,
            "section_count": len(lufs_values),
        }
    return {
        "chorus_verse_delta_lu": round(float(max(lufs_values) - min(lufs_values)), 2),
        "max_section_lufs":      round(float(max(lufs_values)), 2),
        "min_section_lufs":      round(float(min(lufs_values)), 2),
        "section_count":         len(lufs_values),
    }
