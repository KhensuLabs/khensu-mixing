# local-services/khensu_services/analysis/pitch_editor.py
"""Pitch note-grid editor backend.

Two halves of a Melodyne-style "blob" editor:

* ``analyze_pitch_curve`` — pYIN f0 track (downsampled for UI drawing) plus a
  note segmentation: contiguous voiced runs split wherever the median-smoothed
  pitch departs more than 0.75 semitone from the running note centre. Each
  note reports its mean fractional MIDI and the nearest semitone target.
  Pitches are expressed on the *requested* reference grid - A=440 by
  default, or the caller's ``reference_hz`` (e.g. the mix's tuning
  reference) - so snapping corrects onto the grid the rest of the mix uses.
  The take's own reference (circular mean of every voiced frame's offset
  from 12-TET) is still reported as ``detected_reference_hz``, with
  ``reference_offset_warning`` when it sits > 10 cents off the grid; a
  take recorded at A=432 can be snapped onto A=432 by requesting it.
* ``render_pitch_correct`` — applies per-region semitone shifts with
  ``librosa.effects.pitch_shift`` (region + padding, per channel) and
  equal-power crossfades back into the untouched original, so only the edited
  regions change.

Stateless; soundfile for I/O.
"""
from __future__ import annotations

import math
import os
from typing import Any

import numpy as np
import numpy.typing as npt
from khensu_services.analysis.context import load_audio

from khensu_services.analysis._note_names import (
    A4_HZ,
    hz_to_midi,
    tuning_reference_report,
)

MAX_CURVE_POINTS = 4000
_SPLIT_SEMITONES = 0.75
_MIN_NOTE_SEC = 0.060
_SMOOTH_KERNEL = 5          # median-smoothing kernel (frames) for note splitting
_MAX_SHIFT = 12.0
_MIN_SHIFT = 0.01
_PAD_SEC = 0.10             # context around a region fed to the pitch shifter
_FRAME_LENGTH = 2048
_HOP_LENGTH = 256


def _segment_notes(midi: npt.NDArray, voiced: npt.NDArray,
                   hop_sec: float) -> list[dict[str, Any]]:
    """Split voiced runs into notes. ``midi`` is NaN where unvoiced."""
    from scipy.signal import medfilt

    n = len(midi)
    min_frames = max(1, int(math.ceil(_MIN_NOTE_SEC / hop_sec)))
    notes: list[dict[str, Any]] = []

    def _emit(a: int, b: int) -> None:
        if b - a < min_frames:
            return
        vals = midi[a:b]
        vals = vals[np.isfinite(vals)]
        if vals.size == 0:
            return
        m = float(np.mean(vals))
        notes.append({
            "id": len(notes),
            "start_sec": round(a * hop_sec, 4),
            "end_sec": round(b * hop_sec, 4),
            "midi": round(m, 3),
            "target_midi": int(round(m)),
        })

    i = 0
    while i < n:
        if not voiced[i]:
            i += 1
            continue
        j = i
        while j < n and voiced[j]:
            j += 1
        run = midi[i:j]
        k = min(_SMOOTH_KERNEL, len(run) if len(run) % 2 == 1 else len(run) - 1)
        smooth = medfilt(run, kernel_size=k) if k >= 3 else run.copy()
        start = 0
        for t in range(1, len(smooth)):
            centre = float(np.median(smooth[start:t]))
            if abs(float(smooth[t]) - centre) > _SPLIT_SEMITONES:
                _emit(i + start, i + t)
                start = t
        _emit(i + start, j)
        i = j
    return notes


def analyze_pitch_curve(file_path: str, fmin: float = 70.0,
                        fmax: float = 1200.0,
                        reference_hz: float | None = None) -> dict[str, Any]:
    from khensu_services.copilot.dsp import require_finite_audio

    y, sr = load_audio(file_path, sr=None, mono=True)
    require_finite_audio(y, "pitch-curve source")
    sr = int(sr)
    duration = float(len(y) / sr) if sr else 0.0
    hop_sec = _HOP_LENGTH / sr

    if len(y) < _FRAME_LENGTH:
        return {"duration_sec": round(duration, 4), "hop_sec": round(hop_sec, 6),
                "f0_midi": [], "notes": [],
                "reference_hz": round(float(reference_hz or A4_HZ), 2)}

    from khensu_services.analysis import vocal_pitch  # CREPE, else pyin (NEW-7)
    f0, voiced_flag, _ = vocal_pitch.track(
        y, sr, fmin=float(fmin), fmax=float(fmax), hop_length=_HOP_LENGTH, frame_length=_FRAME_LENGTH)
    voiced = np.asarray(voiced_flag, dtype=bool) & np.isfinite(f0)
    tuning = tuning_reference_report(np.where(voiced, f0, np.nan), 1.0 / hop_sec,
                                     reference_hz)
    grid_hz = tuning["measurement_reference_hz"]
    midi = np.full(len(f0), np.nan)
    midi[voiced] = hz_to_midi(f0[voiced], grid_hz)

    notes = _segment_notes(midi, voiced, hop_sec)

    # Downsample the drawable curve to <= MAX_CURVE_POINTS samples.
    factor = max(1, int(math.ceil(len(midi) / MAX_CURVE_POINTS)))
    curve: list[float | None] = []
    for a in range(0, len(midi), factor):
        block = midi[a:a + factor]
        finite = block[np.isfinite(block)]
        if finite.size * 2 >= len(block) and finite.size > 0:
            curve.append(round(float(np.median(finite)), 3))
        else:
            curve.append(None)

    return {
        "duration_sec": round(duration, 4),
        "hop_sec": round(hop_sec * factor, 6),
        "f0_midi": curve,
        "notes": notes,
        # The grid midi/f0_midi/target_midi are on (MIDI 69 == reference_hz).
        "reference_hz": grid_hz,
        **tuning,
    }


def _equal_power_weights(n_total: int, fade: int, a: int, b: int) -> npt.NDArray:
    """Weight (0..1 angle fraction) that is 1 on [a, b) with ``fade``-sample
    ramps outside the region, clamped to [0, n_total)."""
    w = np.zeros(n_total)
    w[a:b] = 1.0
    if fade > 0:
        r0 = max(0, a - fade)
        if a > r0:
            w[r0:a] = np.linspace(0.0, 1.0, a - r0, endpoint=False)
        r1 = min(n_total, b + fade)
        if r1 > b:
            w[b:r1] = np.linspace(1.0, 0.0, r1 - b + 1)[1:]
    return w


def write_render(output_path: str, y: npt.NDArray, sr: int) -> None:
    """Atomic float WAV write; ``y`` is (channels, samples)."""
    import soundfile as sf
    from khensu_services.copilot.dsp import _atomic_render_write

    data = y.T if y.ndim > 1 else y
    _atomic_render_write(output_path,
                         lambda tmp: sf.write(tmp, data, sr, format="WAV", subtype="FLOAT"))


def render_pitch_correct(file_path: str, corrections: list[dict[str, float]],
                         crossfade_ms: float = 10.0,
                         output_path: str | None = None) -> dict[str, Any]:
    import librosa
    import soundfile as sf
    from khensu_services.copilot.dsp import require_finite_audio

    data, sr = sf.read(file_path, always_2d=True, dtype="float64")
    require_finite_audio(data, "pitch-correct source")
    sr = int(sr)
    y = data.T.copy()                       # (channels, samples)
    n = y.shape[1]
    duration = n / sr

    fade = max(0, int(round(max(0.0, float(crossfade_ms)) * 1e-3 * sr)))
    pad = int(round(_PAD_SEC * sr)) + fade
    applied = 0

    for c in corrections:
        start, end = float(c["start_sec"]), float(c["end_sec"])
        shift = float(c["shift_semitones"])
        if not (math.isfinite(start) and math.isfinite(end) and math.isfinite(shift)):
            continue
        shift = float(np.clip(shift, -_MAX_SHIFT, _MAX_SHIFT))
        if abs(shift) < _MIN_SHIFT:
            continue
        start, end = max(0.0, start), min(duration, end)
        a, b = int(round(start * sr)), int(round(end * sr))
        if b - a < 2:
            continue

        s0, s1 = max(0, a - pad), min(n, b + pad)
        w = _equal_power_weights(n, fade, a, b)[s0:s1]
        theta = w * (np.pi / 2.0)
        g_dry, g_wet = np.cos(theta), np.sin(theta)
        for ch in range(y.shape[0]):
            seg = y[ch, s0:s1]
            shifted = librosa.effects.pitch_shift(seg, sr=sr, n_steps=shift)
            shifted = shifted[: len(seg)]
            if len(shifted) < len(seg):
                shifted = np.pad(shifted, (0, len(seg) - len(shifted)))
            y[ch, s0:s1] = seg * g_dry + shifted * g_wet
        applied += 1

    out = output_path or os.path.splitext(file_path)[0] + "_pitchcorrected.wav"
    write_render(out, y, sr)
    return {"rendered_path": out, "applied": applied}
