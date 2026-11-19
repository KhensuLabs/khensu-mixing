"""Shared pitch spelling and pitch-math helpers (music-theory audit #7).

One place for:

* key-aware note spelling - flats for the flat-side keys (F, Bb, Eb, Ab, Db,
  Gb major and their relative minors D, G, C, F, Bb, Eb minor), sharps
  everywhere else, so a Bb-major track reports "Bb major" and chords
  Bb / Eb / F rather than A# / D# / F;
* note-name / key-string parsing;
* Hz <-> MIDI <-> cents conversion, optionally against a non-440 reference;
* circular (mod-one-semitone) cents statistics, used to estimate a track's
  tuning reference without the +-50-cent wrap biasing the mean.

Pure numpy, no librosa import, so it is cheap to import anywhere.
"""
from __future__ import annotations

import math
from typing import Any

import numpy as np
import numpy.typing as npt

SHARP_NAMES: tuple[str, ...] = ("C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B")
FLAT_NAMES: tuple[str, ...] = ("C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B")

# Major-key tonic pitch classes written with flats: F, Bb, Eb, Ab, Db, Gb.
_FLAT_MAJOR_TONICS = frozenset({5, 10, 3, 8, 1, 6})

_LETTER_PC = {"C": 0, "D": 2, "E": 4, "F": 5, "G": 7, "A": 9, "B": 11}

A4_HZ = 440.0
A4_MIDI = 69.0

MODES = ("major", "minor")


def key_uses_flats(tonic_pc: int, mode: str = "major") -> bool:
    """True when the key signature of ``tonic_pc``/``mode`` is flat-side."""
    major_tonic = tonic_pc % 12 if mode != "minor" else (tonic_pc + 3) % 12
    return major_tonic in _FLAT_MAJOR_TONICS


def pc_name(pc: int, tonic_pc: int | None = None, mode: str = "major",
            prefer_flats: bool | None = None) -> str:
    """Spell pitch class ``pc``. Flats when the key (or ``prefer_flats``) says so."""
    if prefer_flats is None:
        prefer_flats = tonic_pc is not None and key_uses_flats(tonic_pc, mode)
    return (FLAT_NAMES if prefer_flats else SHARP_NAMES)[int(pc) % 12]


def key_name(tonic_pc: int, mode: str) -> str:
    """"<Note> major|minor" with the key's own spelling, e.g. "Bb major"."""
    return f"{pc_name(tonic_pc, tonic_pc, mode)} {mode}"


# --- Scale-degree spelling ---------------------------------------------------
#
# Semitones above the tonic -> (accidental, scale degree 1..7), relative to
# the major scale (major keys) or the natural minor scale (minor keys). The
# chromatic choices follow common-practice usage: b2/b3/b6/b7 are borrowed
# (flat) degrees in major, #3/#6/#7 are the raised (melodic/harmonic minor)
# degrees in minor - so the leading tone stays a sharp - and the tritone is
# #4 in both modes. Shared with _harmony.roman_numeral so a chord's spelled
# root and its numeral can never disagree.
DEGREE_MAJOR = {0: ("", 1), 1: ("b", 2), 2: ("", 2), 3: ("b", 3), 4: ("", 3), 5: ("", 4),
                6: ("#", 4), 7: ("", 5), 8: ("b", 6), 9: ("", 6), 10: ("b", 7), 11: ("", 7)}
DEGREE_MINOR = {0: ("", 1), 1: ("b", 2), 2: ("", 2), 3: ("", 3), 4: ("#", 3), 5: ("", 4),
                6: ("#", 4), 7: ("", 5), 8: ("", 6), 9: ("#", 6), 10: ("", 7), 11: ("#", 7)}

_LETTERS = ("C", "D", "E", "F", "G", "A", "B")
_PC_LETTER = {pc: letter for letter, pc in _LETTER_PC.items()}


def degree_of(pc: int, tonic_pc: int, mode: str = "major") -> tuple[str, int]:
    """(accidental, degree 1..7) of pitch class ``pc`` in the key."""
    table = DEGREE_MINOR if (mode or "").strip().lower() == "minor" else DEGREE_MAJOR
    return table[(int(pc) - int(tonic_pc)) % 12]


def _spelled_offset(pc: int, tonic_pc: int, mode: str,
                    tonic_name: str | None = None) -> tuple[str, int]:
    """(spelled note name, semitone offset of its accidental) for ``pc``."""
    tonic = tonic_name or pc_name(tonic_pc, tonic_pc, mode)
    acc, degree = degree_of(pc, tonic_pc, mode)
    letter = _LETTERS[(_LETTERS.index(tonic[0].upper()) + degree - 1) % 7]
    diff = (int(pc) - _LETTER_PC[letter]) % 12
    if diff > 6:
        diff -= 12
    # THY-07: a chromatic (b-degree) note in a flat-side key can land on a
    # double flat (D in Db major = "Ebb", A = "Bbb", G in Gb = "Abb") or on a
    # flattened white key (E in Db = "Fb", B in Bb = "Cb"). Nobody reads a
    # Db-major chart that way: fall back to the single-accidental enharmonic -
    # the natural letter, i.e. the raised #1/#2/#4/#5 reading. Diatonic notes
    # (Cb in Gb major) and the theory-required double sharps of the raised
    # minor degrees (F## in G# minor) keep their degree spelling.
    if acc == "b" and key_uses_flats(tonic_pc, mode) and diff < 0             and (diff <= -2 or letter in ("C", "F")):
        natural = _PC_LETTER.get(int(pc) % 12)
        if natural is not None:
            return natural, 0
    return letter + ("#" * diff if diff > 0 else "b" * -diff), diff


def spell_degree(pc: int, tonic_pc: int, mode: str = "major",
                 tonic_name: str | None = None) -> str:
    """Spell pitch class ``pc`` from its scale degree in the key.

    The letter is the tonic letter advanced by (degree - 1) and the
    accidental is whatever reaches ``pc`` from that letter, using the degree
    the Roman numeral implies (see ``DEGREE_MAJOR``/``DEGREE_MINOR``): bVII in
    C major is "Bb" (not "A#"), bVI is "Ab"; the raised leading tone #vii in
    G minor is "F#" and in D minor "C#" (not "Gb"/"Db"). ``tonic_name``
    overrides the key's own tonic spelling (e.g. "F#" for F# major, which
    ``pc_name`` would write as "Gb"). Double accidentals appear only where
    theory requires them (the leading tone of G# minor is "F##"); a borrowed
    flat degree in a flat key that would need a double flat or a Cb/Fb falls
    back to its natural name (A, not "Bbb", for bVI in Db major).
    """
    return _spelled_offset(pc, tonic_pc, mode, tonic_name)[0]


def midi_name(midi: int | float, tonic_pc: int | None = None, mode: str = "major",
              prefer_flats: bool | None = None, tonic_name: str | None = None) -> str:
    """Scientific pitch name (C4 = MIDI 60) with key-aware spelling.

    With a key, notes are spelled from their scale degree (``spell_degree``),
    so a non-diatonic melody note gets the spelling its function implies -
    C# (the raised leading tone), not Db, in D minor. ``tonic_name`` keeps a
    caller's own tonic spelling for the enharmonic-tie keys (F# major spells
    A#, Gb major Bb). The octave number follows the written letter
    (B#3 == MIDI 60, Cb4 == MIDI 59)."""
    m = int(round(float(midi)))
    if tonic_pc is None or prefer_flats is not None:
        return f"{pc_name(m % 12, tonic_pc, mode, prefer_flats)}{m // 12 - 1}"
    name, offset = _spelled_offset(m % 12, tonic_pc, mode, tonic_name)
    return f"{name}{(m - offset) // 12 - 1}"


def parse_pitch_class(name: str) -> int:
    """Pitch class of a note name such as "C", "f#", "Bb", "E♭", "Cb".

    Raises ValueError on anything that is not a letter A-G followed by any
    number of sharps/flats.
    """
    text = name.strip().replace("♯", "#").replace("♭", "b")
    if not text or text[0].upper() not in _LETTER_PC:
        raise ValueError(f"Unsupported note name {name!r}")
    pc = _LETTER_PC[text[0].upper()]
    for ch in text[1:]:
        if ch == "#":
            pc += 1
        elif ch in ("b", "B"):
            pc -= 1
        else:
            raise ValueError(f"Unsupported note name {name!r}")
    return pc % 12


def parse_key(key: str) -> tuple[int, str] | None:
    """Parse "<Note> major|minor" (also "Am"/"A" shorthand). None if unparseable
    or the "unknown" sentinel."""
    text = (key or "").strip()
    if not text or text.lower() == "unknown":
        return None
    parts = text.split()
    mode = "major"
    if len(parts) >= 2:
        mode = parts[1].lower()
        if mode not in MODES:
            return None
        root = parts[0]
    else:
        root = parts[0]
        if len(root) > 1 and root.endswith("m"):
            root, mode = root[:-1], "minor"
    try:
        return parse_pitch_class(root), mode
    except ValueError:
        return None


# --- Hz / MIDI / cents ------------------------------------------------------

def hz_to_midi(hz: Any, ref_hz: float = A4_HZ) -> Any:
    """Fractional MIDI number for ``hz`` (scalar or array) against ``ref_hz``.

    Non-positive input maps to NaN (scalars too: 0.0 would be a real note,
    C-1, so it can't double as an "unpitched" sentinel).
    """
    if np.isscalar(hz):
        h = float(hz)  # type: ignore[arg-type]
        return A4_MIDI + 12.0 * math.log2(h / ref_hz) if h > 0 else math.nan
    arr = np.asarray(hz, dtype=float)
    with np.errstate(divide="ignore", invalid="ignore"):
        out = A4_MIDI + 12.0 * np.log2(arr / ref_hz)
    return np.where(arr > 0, out, np.nan)


def midi_to_hz(midi: Any, ref_hz: float = A4_HZ) -> Any:
    return ref_hz * np.power(2.0, (np.asarray(midi, dtype=float) - A4_MIDI) / 12.0)


def hz_to_midi_cents(f0: npt.ArrayLike, ref_hz: float = A4_HZ) -> tuple[npt.NDArray, npt.NDArray]:
    """(nearest MIDI note, cents from it) per Hz value; NaN passes through."""
    midi_exact = np.asarray(hz_to_midi(np.asarray(f0, dtype=float), ref_hz), dtype=float)
    nearest = np.round(midi_exact)
    return nearest, (midi_exact - nearest) * 100.0


def cents_to_ref_hz(cents: float) -> float:
    """Concert-A frequency implied by a tuning offset of ``cents`` from A=440."""
    return A4_HZ * (2.0 ** (float(cents) / 1200.0))


REFERENCE_OFFSET_WARN_CENTS = 10.0


def tuning_reference_report(f0_hz: npt.ArrayLike, frame_rate: float | None,
                            reference_hz: float | None = None) -> dict[str, Any]:
    """Which grid to measure a take against, plus what the take itself does.

    Intonation is measured against ``reference_hz`` when the caller supplies
    one (e.g. the mix's tuning reference), else A=440 - never against the
    take's own estimated reference: a take that is uniformly 30 cents flat
    would otherwise read as perfectly in tune. The take's own reference is
    still reported (``detected_reference_hz`` and its offset from A=440 in
    cents) and ``reference_offset_warning`` flags a take that sits more than
    10 cents off the measurement grid.
    """
    detected_offset = float(estimate_reference_offset_cents(f0_hz, frame_rate=frame_rate))
    detected_hz = cents_to_ref_hz(detected_offset)
    grid_hz = float(reference_hz) if reference_hz else A4_HZ
    grid_offset = 1200.0 * math.log2(grid_hz / A4_HZ)
    # Offset of the take from the grid, wrapped to the nearest semitone line.
    offset_vs_grid = float(circular_cents_diff(detected_offset, grid_offset))
    return {
        "measurement_reference_hz": round(grid_hz, 2),
        "detected_reference_hz": round(detected_hz, 2),
        "reference_offset_cents": round(detected_offset, 1),
        "reference_offset_warning": abs(offset_vs_grid) > REFERENCE_OFFSET_WARN_CENTS,
    }


def wrap_cents(cents: Any) -> Any:
    """Wrap cents into [-50, 50) - the distance to the nearest semitone grid line."""
    return (np.asarray(cents, dtype=float) + 50.0) % 100.0 - 50.0


def circular_cents_diff(a: Any, b: Any) -> Any:
    """a - b on the one-semitone circle, in [-50, 50)."""
    return wrap_cents(np.asarray(a, dtype=float) - np.asarray(b, dtype=float))


def circular_mean_cents(cents: npt.ArrayLike) -> float:
    """Circular mean of cents offsets on the 100-cent circle, in [-50, 50).

    An arithmetic mean of wrapped offsets is biased near +-50 (a reference
    49 cents sharp reads half sharp, half flat); averaging unit phasors is not.
    Returns 0.0 for empty / non-finite / fully dispersed input.
    """
    arr = np.asarray(cents, dtype=float)
    arr = arr[np.isfinite(arr)]
    if arr.size == 0:
        return 0.0
    angles = arr * (2.0 * np.pi / 100.0)
    c, s = float(np.mean(np.cos(angles))), float(np.mean(np.sin(angles)))
    if math.hypot(c, s) < 1e-9:
        return 0.0
    return float(wrap_cents(math.atan2(s, c) * 100.0 / (2.0 * np.pi)))


def estimate_reference_offset_cents(f0_hz: npt.ArrayLike, frame_rate: float | None = None,
                                    smooth_sec: float = 0.2, min_frames: int = 20,
                                    min_pitch_classes: int = 3) -> float:
    """Tuning offset (cents from A=440, in [-50, 50)) of a pitched f0 track.

    ``f0_hz`` is a frame track with NaN / <= 0 for unvoiced frames. When
    ``frame_rate`` (frames per second) is given, the fractional-MIDI track is
    first moving-averaged over ``smooth_sec`` inside each voiced run, so each
    frame represents its note's centre rather than an instantaneous vibrato
    excursion - a frame-level circular mean of a +-60-cent vibrato points
    the wrong way (its mean phasor is negative). The circular mean of those
    centre offsets is the reference. With fewer than ``min_frames`` usable
    frames, or fewer than ``min_pitch_classes`` distinct pitch classes, it
    returns 0.0 (assume A=440).
    """
    arr = np.asarray(f0_hz, dtype=float).reshape(-1)
    voiced = np.isfinite(arr) & (arr > 0)
    if int(np.count_nonzero(voiced)) < min_frames:
        return 0.0
    midi = np.full(arr.shape, np.nan)
    midi[voiced] = hz_to_midi(arr[voiced])
    width = int(round(smooth_sec * frame_rate)) if frame_rate else 1
    if width >= 3:
        smoothed = np.full(arr.shape, np.nan)
        n = len(arr)
        i = 0
        while i < n:
            if not voiced[i]:
                i += 1
                continue
            j = i
            while j < n and voiced[j]:
                j += 1
            run = midi[i:j]
            w = min(width, len(run))
            kernel = np.ones(w) / w
            # 'valid' convolution: only fully-supported windows, no edge bias.
            smoothed[i:i + len(run) - w + 1] = np.convolve(run, kernel, mode="valid")
            i = j
        midi = smoothed
    values = midi[np.isfinite(midi)]
    if values.size == 0:
        return 0.0
    # One or two pitches can't separate "the take uses another reference"
    # from "these notes are out of tune": require several distinct pitch
    # classes (each >= 5 % of frames) agreeing before moving off A=440.
    _pcs, counts = np.unique(np.round(values).astype(int) % 12, return_counts=True)
    if int(np.count_nonzero(counts >= 0.05 * values.size)) < min_pitch_classes:
        return 0.0
    return circular_mean_cents((values - np.round(values)) * 100.0)
