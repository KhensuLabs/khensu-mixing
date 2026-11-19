# local-services/khensu_services/analysis/source_triage.py
"""Fix-at-Source Triage: detects recording defects that mixing cannot fully
repair, and classifies each finding by what the user can actually do about it.

The "80% recording / 20% mixing" principle operationalized: before a user
spends hours mixing around a flawed capture, scan the raw track and say
plainly which problems are fixable in the box, which are only mitigable,
and which mean the take should be re-recorded.

Categories:
  fixable   — a standard DSP move fully repairs it (DC offset, hum notch)
  mitigable — tools reduce it but leave artifacts (declip, denoise, EQ)
  rerecord  — baked into the capture; no processing truly removes it
              (heavy clipping, strong early reflections/comb filtering)
  info      — not a defect per se, but worth knowing (format anomalies)

Material (audit 2026-11-06 AID-16): the mic-capture checks (proximity boom,
early-reflection comb filtering) and "re-record" advice only make sense for a
single recorded source or a stem. On a finished mix or master an 808 that
out-weighs the low mids is the genre, not a cardioid worked too close, and
flat-topped peaks are a limiter/bounce problem that is fixed by re-rendering,
not by re-tracking. `triage_source(material=...)` therefore runs the capture
checks only for "single_source"/"stem" and words the rest for mixes/masters.

Stateless, numpy-only (mirrors extended_analyzer.py's style).
"""
from __future__ import annotations

from typing import Any

import numpy as np
import numpy.typing as npt
from khensu_services.analysis.context import load_audio


# ---------------------------------------------------------------------------
# Individual checks — each returns a finding dict or None.
# Finding shape: {code, title, detail, category, severity(0-1)}
# ---------------------------------------------------------------------------

MATERIALS = ("single_source", "stem", "mix", "master")


def _capture_material(material: str) -> bool:
    return material in ("single_source", "stem")


def _run_lengths(mask: npt.NDArray) -> npt.NDArray:
    """Lengths of the True runs in a boolean array (vectorised; AID-16 - the
    per-sample Python loops took seconds on a long file)."""
    m = np.asarray(mask, dtype=np.int8)
    if m.size == 0:
        return np.zeros(0, dtype=np.int64)
    edges = np.diff(np.concatenate(([0], m, [0])))
    starts = np.flatnonzero(edges == 1)
    ends = np.flatnonzero(edges == -1)
    return (ends - starts).astype(np.int64)


def _check_clipping(mono: npt.NDArray, sr: int, material: str = "single_source") -> dict[str, Any] | None:
    """Runs of samples pinned at digital full scale. A handful of 1-2 sample
    events is mitigable with a declipper; sustained flat-topping is baked-in
    distortion no tool truly restores."""
    clipped = np.abs(mono) >= 0.999
    if not bool(np.any(clipped)):
        return None

    # Count runs and the total clipped duration.
    edges = np.diff(clipped.astype(np.int8))
    run_starts = int(np.sum(edges == 1)) + (1 if clipped[0] else 0)
    clipped_ms = float(np.sum(clipped)) / sr * 1000.0

    # Longest single run (samples) — flat-top length is the real damage signal.
    runs = _run_lengths(clipped)
    longest = int(runs.max()) if runs.size else 0

    heavy = clipped_ms > 50.0 or longest > int(sr * 0.002)  # >2 ms flat-top
    severity = float(np.clip(clipped_ms / 200.0 + longest / (sr * 0.01), 0.0, 1.0))
    if not _capture_material(material):
        # A mix/master: the flat tops come from the limiter, clipper or bounce
        # gain, so the remedy is re-rendering, never re-tracking (AID-16).
        return {
            "code": "clipping",
            "title": "Clipped peaks in the mix/master",
            "detail": (f"{run_starts} clip event(s), {clipped_ms:.1f} ms total at full scale, "
                       f"longest flat-top {longest / sr * 1000.0:.2f} ms. "
                       + ("Sustained flat-topping is baked into this file - re-render the "
                          "mix/master with less limiter/clipper drive or a true-peak ceiling "
                          "(e.g. -1 dBTP)." if heavy else
                          "Brief overs - a true-peak limiter on the master bus, or a declipper "
                          "on this file, will tame them.")),
            "category": "mitigable",
            "severity": severity,
        }
    return {
        "code": "clipping",
        "title": "Digital clipping",
        "detail": (f"{run_starts} clip event(s), {clipped_ms:.1f} ms total at full scale, "
                   f"longest flat-top {longest / sr * 1000.0:.2f} ms. "
                   + ("Sustained flat-topping is unrecoverable distortion — re-record with "
                      "5-10 dB more headroom." if heavy else
                      "Brief peaks — a declipper can reconstruct these, but re-tracking "
                      "with more headroom is still cleaner.")),
        "category": "rerecord" if heavy else "mitigable",
        "severity": severity,
    }


def _check_noise_floor(mono: npt.NDArray, sr: int, material: str = "single_source") -> dict[str, Any] | None:
    """10th-percentile RMS of 50 ms windows (same estimator as
    section_analyzer's noise_floor_db). Quiet-passage hiss/room noise gets
    exposed by vocal compression, so the threshold is stricter than 'audible
    in the raw file'."""
    win = max(1, int(0.050 * sr))
    n_win = len(mono) // win
    if n_win < 4:
        return None
    rms = np.sqrt(np.mean(mono[: n_win * win].reshape(n_win, win) ** 2, axis=1) + 1e-20)
    floor_db = float(20.0 * np.log10(np.percentile(rms, 10) + 1e-12))
    p90_db = float(20.0 * np.log10(np.percentile(rms, 90) + 1e-12))

    # A noise *floor* is only observable in the gaps between program material.
    # Continuous material (sustained pads, a constant tone) has no gaps — the
    # 10th percentile is just the signal itself, not noise.
    if p90_db - floor_db < 15.0:
        return None
    if floor_db <= -60.0:
        return None
    very_noisy = floor_db > -40.0
    severity = float(np.clip((floor_db + 60.0) / 30.0, 0.0, 1.0))
    if not _capture_material(material):
        return {
            "code": "noise_floor",
            "title": "High noise floor in quiet passages",
            "detail": (f"Noise floor ~ {floor_db:.1f} dBFS between phrases. Check the noisiest "
                       "tracks/stems and gate or denoise them in the session; on the finished "
                       "file a broadband denoiser can reduce it, with artifacts on tails."),
            "category": "mitigable",
            "severity": severity,
        }
    return {
        "code": "noise_floor",
        "title": "High noise floor",
        "detail": (f"Noise floor ≈ {floor_db:.1f} dBFS. "
                   + ("Loud enough that compression will pump it audibly between phrases — "
                      "treat the room / kill the source and re-record." if very_noisy else
                      "Denoising can reduce it, but expect artifacts on breaths and tails; "
                      "capturing quieter is always cleaner.")),
        "category": "rerecord" if very_noisy else "mitigable",
        "severity": severity,
    }


def _check_hum(mono: npt.NDArray, sr: int) -> dict[str, Any] | None:
    """Mains hum: narrow energy spikes at 50/60 Hz and harmonics, measured
    against the surrounding spectrum. Fully fixable with notch filters."""
    n_fft = 32768
    if len(mono) < n_fft:
        return None
    # Average a few windows from across the file to dodge one-off thumps.
    hops = np.linspace(0, len(mono) - n_fft, num=min(8, len(mono) // n_fft), dtype=int)
    window = np.hanning(n_fft)
    spec = np.zeros(n_fft // 2 + 1)
    for h in hops:
        spec += np.abs(np.fft.rfft(mono[h : h + n_fft] * window))
    spec /= max(1, len(hops))
    freqs = np.fft.rfftfreq(n_fft, d=1.0 / sr)

    spec_max = float(np.max(spec) + 1e-12)

    def peak_ratio(hz: float) -> float:
        band = (freqs >= hz - 2.0) & (freqs <= hz + 2.0)
        around = (freqs >= hz - 15.0) & (freqs <= hz + 15.0) & ~band
        if not np.any(band) or not np.any(around):
            return 0.0
        peak = float(np.max(spec[band]))
        # Absolute-level gate: a "hum peak" thousands of dB below the program
        # is numerically prominent against an even-emptier neighborhood but
        # inaudible — require it to register against the spectrum at all.
        if peak < spec_max * 1e-3:
            return 0.0
        return peak / (float(np.median(spec[around])) + 1e-12)

    for base in (50.0, 60.0):
        ratios = [peak_ratio(base * k) for k in (1, 2, 3)]
        strong = sum(1 for r in ratios if r > 8.0)
        if strong >= 2:
            severity = float(np.clip(max(ratios) / 40.0, 0.2, 1.0))
            return {
                "code": "mains_hum",
                "title": f"Mains hum ({base:.0f} Hz)",
                "detail": (f"Narrow peaks at {base:.0f} Hz and harmonics stand "
                           f"{max(ratios):.0f}× above the surrounding spectrum — a ground "
                           "loop or unbalanced cable. Fully fixable with notch filters at "
                           f"{base:.0f}/{base*2:.0f}/{base*3:.0f} Hz, and worth fixing at the "
                           "source before the next take."),
                "category": "fixable",
                "severity": severity,
            }
    return None


def _dc_offset_steady(mono: npt.NDArray, sr: int, dc: float) -> bool:
    """The offset holds through the file: the 10th and 90th percentiles of its
    one-second means share the mean's sign (the engine's
    ArtifactData::dc_offset_steady, MEAS-4). A file under three seconds has
    only its mean."""
    blocks = len(mono) // max(int(sr), 1)
    if blocks < 3:
        return True
    means = mono[: blocks * int(sr)].reshape(blocks, int(sr)).mean(axis=1)
    p10, p90 = np.percentile(means, [10, 90])
    return bool(p10 > 0 and p90 > 0) if dc >= 0 else bool(p10 < 0 and p90 < 0)


def _check_dc_offset(mono: npt.NDArray, sr: int) -> dict[str, Any] | None:
    dc = float(np.mean(mono))
    # Only a steady offset: half the released tracks a whole-file mean over
    # 0.002 flagged were sub-sonic content swinging both ways (MEAS-4).
    if abs(dc) < 0.002 or not _dc_offset_steady(mono, sr, dc):
        return None
    return {
        "code": "dc_offset",
        "title": "DC offset",
        "detail": (f"Mean sample value {dc:+.4f} — the waveform isn't centered on zero, "
                   "which wastes headroom and can thump on edits. Fully fixable with a "
                   "high-pass filter or DC-removal."),
        "category": "fixable",
        "severity": float(np.clip(abs(dc) * 100.0, 0.1, 1.0)),
    }


def _check_comb_filtering(mono: npt.NDArray, sr: int) -> dict[str, Any] | None:
    """Early-reflection comb filtering: a strong cepstral peak in the
    0.8-12 ms quefrency range (a reflection path 0.3-4 m longer than the
    direct path). Pitch periods for vocals sit above ~3.3 ms (below 300 Hz),
    so the low end of the window can overlap F0 — we require the peak to be
    stable across segments, which reflections are and pitch generally isn't."""
    n_fft = 8192
    seg_len = n_fft
    n_segs = min(10, len(mono) // seg_len)
    if n_segs < 4:
        return None

    q_lo = int(0.0008 * sr)
    q_hi = int(0.012 * sr)
    if q_hi <= q_lo + 2:
        return None

    window = np.hanning(n_fft)
    peak_bins: list[int] = []
    peak_strengths: list[float] = []
    pitch_periods: list[float] = []   # samples, from each segment's dominant partial
    # Sample segments spread across the file, skipping near-silent ones.
    starts = np.linspace(0, len(mono) - seg_len, num=n_segs, dtype=int)
    for s in starts:
        seg = mono[s : s + seg_len]
        if float(np.sqrt(np.mean(seg ** 2))) < 1e-4:
            continue
        mag = np.abs(np.fft.rfft(seg * window))
        log_mag = np.log(mag + 1e-9)
        cep = np.abs(np.fft.irfft(log_mag))
        band = cep[q_lo:q_hi]
        med = float(np.median(band) + 1e-12)
        idx = int(np.argmax(band))
        strength = float(band[idx] / med)
        if strength > 4.0:
            peak_bins.append(idx)
            peak_strengths.append(strength)
            # Dominant partial in the melodic range, for the pitch-period
            # exclusion below (a harmonic signal's cepstrum peaks at 1/F0).
            freqs = np.fft.rfftfreq(n_fft, d=1.0 / sr)
            mel = (freqs >= 60.0) & (freqs <= 1200.0)
            if np.any(mel) and float(np.max(mag[mel])) > 0.1 * float(np.max(mag) + 1e-12):
                f0 = float(freqs[mel][np.argmax(mag[mel])])
                if f0 > 0:
                    pitch_periods.append(sr / f0)

    if len(peak_bins) < 3:
        return None
    # Stability: the same quefrency (±10%) across detections = reflection.
    bins = np.array(peak_bins, dtype=float)
    med_bin = float(np.median(bins))
    stable = np.sum(np.abs(bins - med_bin) < med_bin * 0.10)
    if stable < 3:
        return None

    # Pitch exclusion: harmonic material (a held note, sustained tone) also
    # produces a stable cepstral peak — at the pitch period and its
    # multiples. Only reflections that don't line up with the dominant F0
    # count.
    peak_period_samples = med_bin + q_lo
    if pitch_periods:
        med_pitch = float(np.median(pitch_periods))
        for mult in (1.0, 2.0, 3.0, 0.5):
            if abs(peak_period_samples - med_pitch * mult) < med_pitch * mult * 0.15:
                return None

    delay_ms = (med_bin + q_lo) / sr * 1000.0
    distance_m = delay_ms / 1000.0 * 343.0
    return {
        "code": "comb_filtering",
        "title": "Early reflection / comb filtering",
        "detail": (f"A stable spectral ripple with ~{delay_ms:.1f} ms delay (reflection path "
                   f"~{distance_m:.1f} m longer than the direct sound) — usually a desk, "
                   "window, or untreated wall near the mic. EQ cannot undo comb filtering; "
                   "re-record with the mic repositioned or the surface treated."),
        "category": "rerecord",
        "severity": float(np.clip(np.mean(peak_strengths) / 12.0, 0.3, 1.0)),
    }


def _check_proximity_boom(mono: npt.NDArray, sr: int) -> dict[str, Any] | None:
    """Excess low-frequency tilt typical of singing too close to a cardioid
    mic: 60-200 Hz energy far above the 200-1000 Hz body of the voice."""
    n_fft = 16384
    if len(mono) < n_fft:
        return None
    hops = np.linspace(0, len(mono) - n_fft, num=min(8, len(mono) // n_fft), dtype=int)
    window = np.hanning(n_fft)
    spec = np.zeros(n_fft // 2 + 1)
    for h in hops:
        spec += np.abs(np.fft.rfft(mono[h : h + n_fft] * window)) ** 2
    freqs = np.fft.rfftfreq(n_fft, d=1.0 / sr)

    low = float(np.sum(spec[(freqs >= 60.0) & (freqs < 200.0)]) + 1e-12)
    body = float(np.sum(spec[(freqs >= 200.0) & (freqs < 1000.0)]) + 1e-12)
    ratio_db = 10.0 * np.log10(low / body)

    if ratio_db < 3.0:
        return None
    return {
        "code": "proximity_boom",
        "title": "Proximity-effect boom",
        "detail": (f"60-200 Hz energy is {ratio_db:.1f} dB above the 200 Hz-1 kHz body — "
                   "typical of working a cardioid mic too close. A high-pass/low-shelf cut "
                   "mitigates it, but the tone change from mic distance can't be fully "
                   "EQ'd back; consider re-tracking 10-20 cm further off the mic."),
        "category": "mitigable",
        "severity": float(np.clip((ratio_db - 3.0) / 9.0, 0.1, 1.0)),
    }


def _check_dropouts(mono: npt.NDArray, sr: int, material: str = "single_source") -> dict[str, Any] | None:
    """Hard zero-runs mid-signal (buffer dropouts / bad edits). Interior
    only — leading/trailing silence is normal."""
    nz = np.flatnonzero(np.abs(mono) > 1e-6)
    if nz.size < 2:
        return None
    interior = np.abs(mono[nz[0] : nz[-1]]) < 1e-7
    if not bool(np.any(interior)):
        return None
    # Runs of exact silence >= 5 ms inside the program are suspicious.
    min_run = max(1, int(0.005 * sr))
    runs = int(np.sum(_run_lengths(interior) >= min_run))
    if runs == 0:
        return None
    if not _capture_material(material):
        return {
            "code": "dropouts",
            "title": "Dropouts / dead samples",
            "detail": (f"{runs} run(s) of exact digital silence >= 5 ms inside the program - "
                       "a render glitch, a bad edit or an export buffer underrun. Re-export the "
                       "mix/master from the session, and check the edits at those spots."),
            "category": "mitigable",
            "severity": float(np.clip(runs / 5.0, 0.2, 1.0)),
        }
    return {
        "code": "dropouts",
        "title": "Dropouts / dead samples",
        "detail": (f"{runs} run(s) of exact digital silence ≥ 5 ms inside the program — "
                   "buffer underruns during capture or unhealed edits. If these fall on "
                   "musical material, that audio is gone: patch from another take or "
                   "re-record the affected passage."),
        "category": "rerecord",
        "severity": float(np.clip(runs / 5.0, 0.2, 1.0)),
    }


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

_CATEGORY_ORDER = {"rerecord": 0, "mitigable": 1, "fixable": 2, "info": 3}


def detect_material(file_path: str, n_channels: int | None = None) -> str:
    """Best guess at what `file_path` is (used when the caller says "auto"):
    a separated stem (its folder carries a separation.json, or the file is
    named like a stem), a single mono source, else a stereo mix."""
    import os
    from khensu_services.analysis.separator import STEM_NAMES, read_separation_provenance
    folder = os.path.dirname(os.path.abspath(file_path))
    stem = os.path.splitext(os.path.basename(file_path))[0].lower()
    if read_separation_provenance(folder) is not None and stem in STEM_NAMES:
        return "stem"
    if n_channels is None:
        try:
            import soundfile as sf
            n_channels = int(sf.info(file_path).channels)
        except Exception:
            n_channels = 2
    return "single_source" if n_channels == 1 else "mix"


def triage_cache_params(material: str) -> dict[str, str] | None:
    """The content-hash cache's parameters for a triage of `material`
    (CON-7): the route and the full intake use the same, so the Dashboard's
    triage of the session's mix reads what the intake measured."""
    return None if material == "auto" else {"material": material}


def triage_source(file_path: str, ctx=None, material: str = "auto") -> dict[str, Any]:
    """Run all fix-at-source checks plus the existing file-integrity checks.

    Returns { file_path, duration_sec, findings: [...], verdict } where
    findings are sorted worst-category-first then by severity, and verdict is
    a one-line overall call: "clean" / "fix in the box" / "mitigate" /
    "re-record recommended".
    """

    from khensu_services.analysis.integrity_checker import check_file_integrity

    # ``ctx`` is an optional shared AnalysisContext (full intake passes one):
    # its mono decode is exactly librosa.load(sr=None, mono=True).
    if ctx is not None:
        y, sr = ctx.load(sr=None, mono=True)
    else:
        y, sr = load_audio(file_path, sr=None, mono=True)
    sr = int(sr)
    if material not in MATERIALS:
        material = detect_material(file_path)
    capture = _capture_material(material)

    findings: list[dict[str, Any]] = []
    checks = [
        lambda: _check_clipping(y, sr, material),
        lambda: _check_dropouts(y, sr, material),
        lambda: _check_noise_floor(y, sr, material),
        lambda: _check_hum(y, sr),
        lambda: _check_dc_offset(y, sr),
    ]
    if capture:
        # Mic-capture checks: only meaningful on a single recorded source or a
        # stem (AID-16) - an 808-heavy master is not proximity effect.
        checks[2:2] = [lambda: _check_comb_filtering(y, sr),
                       lambda: _check_proximity_boom(y, sr)]
    for check in checks:
        result = check()
        if result is not None:
            findings.append(result)

    # Fold in integrity warnings as informational findings.
    try:
        integrity = check_file_integrity(file_path)
        for w in integrity.get("warnings", []):
            findings.append({
                "code": "integrity",
                "title": "File integrity",
                "detail": w,
                "category": "info",
                "severity": 0.2,
            })
    except Exception:
        pass  # integrity layer is best-effort here; audio checks already ran

    findings.sort(key=lambda f: (_CATEGORY_ORDER.get(f["category"], 9), -f["severity"]))

    # CE-TRU-1: say whether each finding's rule has been measured against a
    # labelled corpus ("calibrated", n / precision) or not ("heuristic").
    from khensu_services.common.calibration import confidence_basis
    for f in findings:
        f["confidence_basis"] = confidence_basis(f["code"], f["severity"])

    categories = {f["category"] for f in findings}
    if "rerecord" in categories and capture:
        verdict = "re-record recommended"
    elif "mitigable" in categories:
        verdict = "mitigate before mixing"
    elif "fixable" in categories:
        verdict = "fix in the box"
    else:
        verdict = "clean"

    return {
        "file_path": file_path,
        "duration_sec": float(len(y) / sr) if sr > 0 else 0.0,
        "findings": findings,
        "verdict": verdict,
    }
