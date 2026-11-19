"""DSP regression harness — guards analyzer numeric output against drift.

Unlike the per-analyzer unit tests (which assert loose ranges like "harshness
< 0.5"), this pins a *fingerprint* of several analyzers on fixed synthetic
signals to stored golden values with tight tolerances. A refactor that
silently shifts a score by 0.1 passes the range tests but fails here.

What is pinned (MEAS-2 widened it from loudness / harshness / mono-compat):

* on the three 4 s test tones (identical left and right, 22.05 kHz):
  integrated LUFS, LRA, harshness, mid-band correlation, the short-term and
  momentary maxima, true peak, crest, stereo width and correlation; on the
  noise also spectral tilt and the low-band share;
* on two generated songs (``_music_signals.py``: chords, bass, drums and a
  bed, different left and right, 44.1 kHz, 32 s and 40 s): all of the above
  plus the estimated key and its confidence, tempo, time signature, how many
  sections were found and where the first boundary is, and for the A minor
  song the chord key and the chords of its first four bars.

Numbers are compared with a tolerance per unit (``_TOL_BY_SUFFIX``); names,
labels and counts must match exactly.

Left out on purpose, as too easy to move with a library or platform change
without any analyzer change: tilt and low-band share of the pure tones (the
octave bands hold only window leakage), key / tempo / sections of the tones
and the noise (no music to find), the number of tracked beats and the time
of the first beat, the downbeat phase, per-segment chord confidences, and
the chords of the C major song (its IV and V read as "no chord": the pad's
third harmonics make them ambiguous, and labels at that margin are not a
stable pin).

Regenerate the golden file after an intentional algorithm change:

    KHENSU_REGEN_GOLDEN=1 python -m pytest tests/test_golden_regression.py

and commit tests/golden/analyzer_fingerprint.json alongside the change so the
diff shows exactly which metrics moved.
"""
from __future__ import annotations

import json
import os
from pathlib import Path

import numpy as np
import pytest

import _music_signals as ms

GOLDEN_PATH = Path(__file__).parent / "golden" / "analyzer_fingerprint.json"

SR = 22050
DURATION = 4.0  # >= a couple of 3 s LUFS windows

# The generated songs: 44.1 kHz (at 22.05 kHz their hats alias), four loops
# so there is more than one section to find.
SONG_SR = 44100
SONG_LOOPS = 4
# Songs whose chord labels are pinned (see the module docstring).
_CHORD_SONGS = ("a_minor_96",)

# Absolute tolerance per metric-key suffix. Scores are 0..1 (tight); dB/LUFS
# values get a wider band; frequencies wider still. First match wins, so new
# suffixes go after the original six and cannot change their keys' tolerance.
_TOL_BY_SUFFIX = {
    "score": 0.01,
    "corr": 0.01,
    "lufs": 0.1,
    "lra": 0.1,
    "db": 0.15,
    "hz": 5.0,
    "dbtp": 0.1,         # true peak
    "octave": 0.05,      # spectral tilt, dB/octave
    "ratio": 0.005,      # low-band share of the power, 0..1
    "width": 0.01,       # stereo width, 0..1
    "confidence": 0.02,  # key confidence, 0..1
    "bpm": 0.5,
    "sec": 0.1,          # a section boundary; one beat is 0.5 s or more
}
_DEFAULT_TOL = 0.02


def _tol_for(key: str) -> float:
    for suffix, tol in _TOL_BY_SUFFIX.items():
        if key.endswith(suffix):
            return tol
    return _DEFAULT_TOL


def _signals() -> dict[str, tuple[np.ndarray, int]]:
    """Deterministic synthetic signals. Any randomness is seeded so the
    fingerprint is reproducible across machines and runs."""
    t = np.linspace(0.0, DURATION, int(DURATION * SR), endpoint=False)
    rng = np.random.default_rng(42)

    sine_1k = 0.5 * np.sin(2 * np.pi * 1000 * t)
    two_tone = 0.4 * np.sin(2 * np.pi * 1000 * t) + 0.4 * np.sin(2 * np.pi * 3500 * t)
    noise = 0.2 * rng.standard_normal(t.shape[0])

    def stereo(mono: np.ndarray) -> np.ndarray:
        return np.stack([mono, mono]).astype(np.float64)

    return {
        "sine_1k": (stereo(sine_1k), SR),
        "two_tone": (stereo(two_tone), SR),
        "noise": (stereo(noise), SR),
    }


def _integrated_lufs(y_stereo: np.ndarray, sr: int) -> float:
    # The shared meter (ITU BS.1770-4 K-weighting since audit 2026-11-06
    # CORE-12), so a change to the production loudness path moves the golden.
    from khensu_services.analysis.loudness import integrated_lufs

    val = integrated_lufs(y_stereo, sr)
    return float(val) if val is not None and np.isfinite(val) else -70.0


def _add_shared_measurements(fp: dict[str, float | int | str], prefix: str, y: np.ndarray, sr: int,
                             *, spectral: bool) -> None:
    """Peak, dynamics and stereo field through the shared measurement code
    (``analysis/loudness.py``, ``analysis/dsp_metrics.py``). ``spectral``
    adds tilt and the low-band share - only meaningful on broadband signals."""
    from khensu_services.analysis import dsp_metrics, loudness

    short_term = loudness.short_term_loudness_series(y, sr)
    momentary = loudness.short_term_loudness_series(y, sr, block_sec=0.4)
    fp[f"{prefix}.short_term_max_lufs"] = round(float(np.nanmax(short_term)), 4)
    fp[f"{prefix}.momentary_max_lufs"] = round(float(np.nanmax(momentary)), 4)
    fp[f"{prefix}.true_peak_dbtp"] = round(float(loudness.true_peak_dbtp(y, sr)), 4)
    fp[f"{prefix}.crest_db"] = round(float(loudness.crest_factor_db(y)), 4)
    fp[f"{prefix}.stereo_width"] = round(float(dsp_metrics.stereo_width(y[0], y[1])), 4)
    fp[f"{prefix}.stereo_corr"] = round(float(dsp_metrics.stereo_correlation(y[0], y[1])), 4)
    if spectral:
        fp[f"{prefix}.tilt_db_per_octave"] = round(float(dsp_metrics.spectral_tilt_db_per_octave(y, sr)), 4)
        fp[f"{prefix}.low_band_ratio"] = round(float(dsp_metrics.low_band_energy_ratio(y.mean(axis=0), sr)), 4)


def _add_song(fp: dict[str, float | int | str], name: str) -> None:
    """One generated song: levels and stereo field, then what the music
    analyzers make of it - key, tempo, meter, sections, chords."""
    import librosa

    from khensu_services.analysis import _key
    from khensu_services.analysis import _lean_spectral as _lean
    from khensu_services.analysis import _shared_analysis as shared
    from khensu_services.analysis.loudness import loudness_range
    from khensu_services.analysis.performance_analyzer import detect_chord_sequence
    from khensu_services.analysis.section_analyzer import detect_sections

    sr = SONG_SR
    y = ms.song(name, sr, loops=SONG_LOOPS)
    mono = y.mean(axis=0)
    prefix = f"song_{name}"

    fp[f"{prefix}.integrated_lufs"] = round(_integrated_lufs(y, sr), 4)
    fp[f"{prefix}.lra"] = round(float(loudness_range(y, sr)), 4)
    _add_shared_measurements(fp, prefix, y, sr, spectral=True)

    # Key: the quick analyzer's chroma (hop 4096) through the shared estimator.
    details = _key.estimate_key_details(_key.safe_chroma_cqt(mono, sr, hop_length=4096).mean(axis=1))
    fp[f"{prefix}.key"] = str(details["key"])
    fp[f"{prefix}.key_confidence"] = round(float(details["confidence"]), 4)

    # Tempo and meter: the tracker path the analyzers share.
    tempo, frames = _lean.beat_track(y=mono, sr=sr)
    beat_times = librosa.frames_to_time(frames, sr=sr).tolist()
    bpm = float(shared.tracked_tempo(tempo, beat_times))
    fp[f"{prefix}.tempo_bpm"] = round(bpm, 2)
    fp[f"{prefix}.time_signature"] = str(shared.estimate_meter(mono, sr, beat_times, bpm)["time_signature"])

    sections = detect_sections(y, sr)
    fp[f"{prefix}.section_count"] = len(sections)
    # -1.0 when the whole track is one section, so the key is always there.
    fp[f"{prefix}.first_boundary_sec"] = round(float(sections[1]["start_sec"]), 2) if len(sections) > 1 else -1.0

    if name in _CHORD_SONGS:
        hop = 512  # the chords route's resolution
        seq = detect_chord_sequence(_key.safe_chroma_cqt(mono, sr, hop_length=hop), sr, hop)
        # Half-second segments whose middle lies in the first four bars.
        first_loop = [c["chord"] for c in seq["chords"] if c["time_sec"] + 0.25 < ms.loop_seconds(name)]
        changes = [lab for i, lab in enumerate(first_loop) if i == 0 or lab != first_loop[i - 1]]
        fp[f"{prefix}.chord_key"] = f"{seq['key']} {seq['mode']}".strip()
        fp[f"{prefix}.chord_progression"] = " ".join(changes)


def _fingerprint() -> dict[str, float | int | str]:
    """Flat, dotted-key map of stable analyzer outputs on the fixed signals."""
    from khensu_services.analysis.loudness import loudness_range
    from khensu_services.analysis.extended_analyzer import (
        analyze_harshness,
        analyze_mono_compat,
    )

    sigs = _signals()
    fp: dict[str, float | int | str] = {}

    for name, (y, sr) in sigs.items():
        fp[f"{name}.integrated_lufs"] = round(_integrated_lufs(y, sr), 4)
        fp[f"{name}.lra"] = round(float(loudness_range(y, sr)), 4)

        harsh = analyze_harshness(y, sr)
        fp[f"{name}.harshness_score"] = round(float(harsh["score"]), 4)
        # peak_hz is None when nothing sits in the 2.5-5 kHz harsh band
        # (e.g. the 1 kHz sine) — map that to a sentinel so it stays stable.
        peak_hz = harsh["peak_hz"]
        fp[f"{name}.harshness_peak_hz"] = round(float(peak_hz), 2) if peak_hz is not None else -1.0

        mono = analyze_mono_compat(y, sr)
        # Identical L/R -> correlation 1.0 across bands; a good invariant.
        fp[f"{name}.midrange_corr"] = round(
            float(mono["per_band_correlation"]["midrange"]), 4
        )

        # Tilt and low-band share need a broadband signal: on a pure tone the
        # other octave bands hold only window leakage.
        _add_shared_measurements(fp, name, y, sr, spectral=(name == "noise"))

    for song in sorted(ms.SONGS):
        _add_song(fp, song)

    return fp


def _differs(key: str, golden: float | int | str, current: float | int | str) -> str | None:
    """Why ``current`` is not the golden value, or None when it is close
    enough. Labels and counts must be equal; numbers get the key's tolerance."""
    exact = isinstance(golden, (str, bool)) or isinstance(current, (str, bool)) or key.endswith("count")
    if exact:
        if type(golden) is not type(current) or golden != current:
            return f"  {key}: golden={golden!r} current={current!r} (must be equal)"
        return None
    tol = _tol_for(key)
    delta = abs(current - golden)
    if not delta <= tol:   # also catches NaN
        return f"  {key}: golden={golden} current={current} (|Δ|={delta:.4f} > tol={tol})"
    return None


def test_no_analyzer_drift():
    current = _fingerprint()

    if os.environ.get("KHENSU_REGEN_GOLDEN") == "1":
        GOLDEN_PATH.parent.mkdir(parents=True, exist_ok=True)
        GOLDEN_PATH.write_text(json.dumps(current, indent=2, sort_keys=True) + "\n")
        pytest.skip(f"Regenerated golden fingerprint at {GOLDEN_PATH}")

    assert GOLDEN_PATH.exists(), (
        f"Missing golden file {GOLDEN_PATH}. Generate it once with "
        f"KHENSU_REGEN_GOLDEN=1 python -m pytest {Path(__file__).name}"
    )
    golden = json.loads(GOLDEN_PATH.read_text())

    missing = sorted(set(golden) - set(current))
    added = sorted(set(current) - set(golden))
    assert not missing, f"Golden metrics absent from current run: {missing}"
    assert not added, f"New metrics not in golden (regenerate): {added}"

    drift = [why for key, gval in golden.items() if (why := _differs(key, gval, current[key])) is not None]
    assert not drift, "Analyzer output drifted beyond tolerance:\n" + "\n".join(drift)
