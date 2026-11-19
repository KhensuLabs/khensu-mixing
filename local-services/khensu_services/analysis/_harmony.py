"""Key-relationship and functional-harmony helpers (music-theory outputs).

Pure functions over pitch classes - no numpy/librosa - so they are cheap to
import anywhere:

* :func:`camelot_code` - the DJ "Camelot wheel" code of a key (8B = C major,
  8A = A minor; adjacent numbers are a fifth apart, same number A<->B is the
  relative major/minor);
* :func:`circle_of_fifths_index` - clockwise position on the circle of
  fifths, 0..11 from C major / A minor (G/Em = 1 ... F/Dm = 11);
* :func:`relative_key` / :func:`parallel_key` - spelled with the target
  key's own spelling (so the parallel minor of Db major is "C# minor");
* :func:`roman_numeral` - Roman-numeral label of a chord symbol relative to
  the key (case = chord quality, accidental prefix = chromatic root relative
  to the major / natural-minor scale, e.g. "bVII" for a borrowed Bb in C);
* :func:`is_diatonic` - whether the chord's tones all sit in the key
  (natural minor, or harmonic minor for its raised leading tone);
* :func:`chord_function` - the chord's harmonic category: "diatonic",
  "secondary" (an applied dominant / leading-tone chord of a diatonic
  target), "borrowed" (modal mixture from the parallel mode) or
  "chromatic" (anything else).

Display strings are ASCII-only ("o" for diminished, "b"/"#" accidentals):
JUCE renders multibyte UTF-8 as per-byte mojibake on some setups.
"""
from __future__ import annotations

from typing import Any

from khensu_services.analysis._note_names import (
    DEGREE_MAJOR as _DEGREE_MAJOR,
    DEGREE_MINOR as _DEGREE_MINOR,
    key_uses_flats,
    parse_pitch_class,
    pc_name,
)

NO_CHORD = "N"

# Chord-quality suffix (as emitted by performance_analyzer.detect_chord_sequence)
# -> (intervals above root, numeral case, numeral suffix).
_QUALITY: dict[str, tuple[tuple[int, ...], str, str]] = {
    "": ((0, 4, 7), "upper", ""),
    "m": ((0, 3, 7), "lower", ""),
    "7": ((0, 4, 7, 10), "upper", "7"),
    "maj7": ((0, 4, 7, 11), "upper", "maj7"),
    "m7": ((0, 3, 7, 10), "lower", "7"),
    "dim": ((0, 3, 6), "lower", "o"),
    "dim7": ((0, 3, 6, 9), "lower", "o7"),
    # Half-diminished: ASCII "m7b5" (not "o7", which is the FULLY diminished
    # seventh - the two chords differ in their 7th).
    "m7b5": ((0, 3, 6, 10), "lower", "m7b5"),
    "aug": ((0, 4, 8), "upper", "+"),
    "sus2": ((0, 2, 7), "upper", "sus2"),
    "sus4": ((0, 5, 7), "upper", "sus4"),
}

_MAJOR_SCALE = (0, 2, 4, 5, 7, 9, 11)
_NATURAL_MINOR_SCALE = (0, 2, 3, 5, 7, 8, 10)
_HARMONIC_MINOR_SCALE = (0, 2, 3, 5, 7, 8, 11)

# Degree tables (semitones above the tonic -> (accidental, degree)) live in
# _note_names so chord-root spelling (spell_degree) and the numeral agree.
_ROMAN = ("I", "II", "III", "IV", "V", "VI", "VII")


def _norm_mode(mode: str) -> str:
    return "minor" if (mode or "").strip().lower() == "minor" else "major"


def circle_of_fifths_index(tonic_pc: int, mode: str = "major") -> int:
    """Clockwise circle-of-fifths position, 0..11: C major / A minor = 0,
    G / E minor = 1, ..., F / D minor = 11 (relative keys share a position)."""
    major_pc = tonic_pc % 12 if _norm_mode(mode) == "major" else (tonic_pc + 3) % 12
    return (major_pc * 7) % 12


def camelot_code(tonic_pc: int, mode: str = "major") -> str:
    """Camelot wheel code: "8B" = C major, "8A" = A minor, "1B" = B major."""
    number = (circle_of_fifths_index(tonic_pc, mode) + 7) % 12 + 1
    return f"{number}{'A' if _norm_mode(mode) == 'minor' else 'B'}"


# Keys whose two enharmonic spellings carry the same number of accidentals
# (6 sharps vs 6 flats): F#/Gb major and D#/Eb minor. Neither spelling is
# "the" key's own, so a related key here follows the SOURCE key's
# accidental preference instead of a fixed default.
_ENHARMONIC_TIES = {("major", 6), ("minor", 3)}


def _related_key_name(pc: int, mode: str, source_flats: bool | None) -> str:
    pc %= 12
    if source_flats is not None and (mode, pc) in _ENHARMONIC_TIES:
        return f"{pc_name(pc, prefer_flats=source_flats)} {mode}"
    return f"{pc_name(pc, pc, mode)} {mode}"


def _source_flats(tonic_pc: int, mode: str, prefer_flats: bool | None) -> bool:
    return key_uses_flats(tonic_pc, _norm_mode(mode)) if prefer_flats is None else prefer_flats


def relative_key(tonic_pc: int, mode: str = "major", prefer_flats: bool | None = None) -> str:
    """Relative major/minor (same key signature), e.g. C major -> "A minor".
    ``prefer_flats`` is the source key's spelling (None: its key signature)."""
    flats = _source_flats(tonic_pc, mode, prefer_flats)
    if _norm_mode(mode) == "major":
        return _related_key_name(tonic_pc + 9, "minor", flats)
    return _related_key_name(tonic_pc + 3, "major", flats)


def parallel_key(tonic_pc: int, mode: str = "major", prefer_flats: bool | None = None) -> str:
    """Parallel key (same tonic, other mode), e.g. C major -> "C minor";
    F# minor -> "F# major" (not "Gb major": a 6/6 tie keeps the source's
    sharps), while Db major -> "C# minor" (Db minor would need 8 flats)."""
    other = "minor" if _norm_mode(mode) == "major" else "major"
    return _related_key_name(tonic_pc, other, _source_flats(tonic_pc, mode, prefer_flats))


def key_relations(tonic_pc: int, mode: str = "major",
                  prefer_flats: bool | None = None) -> dict[str, Any]:
    """All key-level outputs in one dict (field names match the API).
    ``prefer_flats``: the source key string's own accidental (e.g. False for
    "F# major", True for "Gb major"); None derives it from the key."""
    return {
        "camelot": camelot_code(tonic_pc, mode),
        "circle_of_fifths_index": circle_of_fifths_index(tonic_pc, mode),
        "relative_key": relative_key(tonic_pc, mode, prefer_flats),
        "parallel_key": parallel_key(tonic_pc, mode, prefer_flats),
    }


def parse_chord(chord: str) -> tuple[int, str] | None:
    """(root pitch class, quality suffix) of a chord symbol such as "Bbm7",
    "F#dim", "Csus4". None for "N" / empty / unrecognised symbols."""
    text = (chord or "").strip()
    if not text or text == NO_CHORD:
        return None
    i = 1
    while i < len(text) and text[i] in "#b":
        # "Bb" is a flat root, but in "Bbm" the b is still the accidental;
        # no supported quality starts with "b", so this is unambiguous.
        i += 1
    root, quality = text[:i], text[i:]
    if quality not in _QUALITY:
        return None
    try:
        return parse_pitch_class(root), quality
    except ValueError:
        return None


def _chord_pcs(root_pc: int, quality: str) -> frozenset[int]:
    return frozenset((root_pc + iv) % 12 for iv in _QUALITY[quality][0])


def is_diatonic(chord: str, tonic_pc: int, mode: str = "major") -> bool | None:
    """True when every chord tone is in the key's scale. Minor keys accept the
    natural or the harmonic minor (so V and viio with the raised leading tone
    count as diatonic). None for "N" / unparseable chords."""
    parsed = parse_chord(chord)
    if parsed is None:
        return None
    pcs = {(pc - tonic_pc) % 12 for pc in _chord_pcs(*parsed)}
    if _norm_mode(mode) == "major":
        return pcs <= set(_MAJOR_SCALE)
    return pcs <= set(_NATURAL_MINOR_SCALE) or pcs <= set(_HARMONIC_MINOR_SCALE)


# Chords a secondary dominant can tonicize (semitones above the tonic ->
# numeral of the target): the diatonic major/minor triads other than the tonic
# itself, never a diminished one. Minor keys use the harmonic-minor V.
_TONICIZABLE = {
    "major": {2: "ii", 4: "iii", 5: "IV", 7: "V", 9: "vi"},
    # VII (the subtonic major triad) is tonicizable too: D7 in A minor is
    # V7/VII, not "IV7" (THY-10).
    "minor": {3: "III", 5: "iv", 7: "V", 8: "VI", 10: "VII"},
}
_DIMINISHED_QUALITIES = frozenset({"dim", "dim7", "m7b5"})


def roman_numeral(chord: str, tonic_pc: int, mode: str = "major") -> str | None:
    """Roman-numeral label of ``chord`` in the key ``tonic_pc``/``mode``.

    Upper case for major-third chords (and sus chords), lower case for minor
    and diminished; suffixes "7", "maj7", "o" (diminished), "o7" (fully
    diminished 7th), "m7b5" (half-diminished, e.g. "viim7b5"), "+",
    "sus2"/"sus4". The root's accidental is relative to the major scale in
    major keys and the natural minor scale in minor keys, so borrowed /
    chromatic roots are marked: Bb in C major -> "bVII", Ab -> "bVI",
    F# dim in C -> "#ivo"; in A minor C -> "III", G -> "VII", E -> "V".

    Common-practice conventions on top of that (AUDIT-084):

    * in minor, the diminished chord on the raised leading tone is "viio"
      (G#dim in A minor - the harmonic-minor vii, not "#viio");
    * a dominant seventh that resolves down a fifth to a diatonic chord
      other than the tonic is a secondary dominant: D7 in C -> "V7/V",
      E7 -> "V7/vi", C7 -> "V7/IV"; B7 in A minor -> "V7/V" (not "II7"),
      G7 -> "V7/III". The key's own dominant stays "V7".

    Returns None for "N" (no chord) or unparseable input.
    """
    parsed = parse_chord(chord)
    if parsed is None:
        return None
    root_pc, quality = parsed
    _ivs, case, suffix = _QUALITY[quality]
    norm = _norm_mode(mode)
    offset = (root_pc - tonic_pc) % 12
    if quality == "7" and offset != 7:
        target = _TONICIZABLE[norm].get((offset + 5) % 12)
        if target is not None:
            return f"V7/{target}"
    table = _DEGREE_MAJOR if norm == "major" else _DEGREE_MINOR
    accidental, degree = table[offset]
    if norm == "minor" and offset == 11 and quality in _DIMINISHED_QUALITIES:
        accidental = ""
    numeral = _ROMAN[degree - 1]
    if case == "lower":
        numeral = numeral.lower()
    return f"{accidental}{numeral}{suffix}"


# --- Chord function category (THY-05) ----------------------------------------

DIATONIC = "diatonic"
SECONDARY = "secondary"
BORROWED = "borrowed"
CHROMATIC = "chromatic"

# Parallel-mode collections a borrowed chord may come from. Major keys borrow
# from the parallel minor (natural and harmonic: iv, bVI, bVII, bIII, iio,
# the fully diminished viio7); minor keys from the parallel major and the
# ascending melodic minor (IV, ii, the Picardy I).
_MELODIC_MINOR_SCALE = (0, 2, 3, 5, 7, 9, 11)
_BORROW_SOURCES = {
    "major": (_NATURAL_MINOR_SCALE, _HARMONIC_MINOR_SCALE),
    "minor": (_MAJOR_SCALE, _MELODIC_MINOR_SCALE),
}
_MAJOR_TRIAD_QUALITIES = frozenset({"", "7"})


def _is_secondary(root_offset: int, quality: str, norm: str) -> bool:
    """Applied chord of a diatonic, non-tonic, non-diminished target: a major
    (or dominant-7th) chord a fifth above it (V/x) or a diminished chord a
    semitone below it (viio/x)."""
    targets = _TONICIZABLE[norm]
    if quality in _MAJOR_TRIAD_QUALITIES:
        return (root_offset + 5) % 12 in targets
    if quality in _DIMINISHED_QUALITIES:
        return (root_offset + 1) % 12 in targets
    return False


def chord_function(chord: str, tonic_pc: int, mode: str = "major") -> str | None:
    """Harmonic category of ``chord`` in the key: "diatonic", "secondary",
    "borrowed" or "chromatic"; None for "N" / unparseable input.

    * "secondary": an applied chord - the dominant (seventh) a fifth above a
      diatonic target or the diminished chord a semitone below one. D7 in C
      (V7/V), E7 (V7/vi), F#dim (viio/V), D major in C (V/V). Every numeral
      :func:`roman_numeral` writes as "V7/x" is secondary.
    * "borrowed": modal mixture - every tone is in the parallel mode (Fm,
      Ab, Bb, Eb in C major; D major (IV) or the Picardy A major in A minor).
    * "chromatic": anything else (the Neapolitan Db in C reads borrowed only
      if it fits a parallel collection; F# major in C is chromatic).

    A dominant seventh or diminished chord is checked for an applied function
    before mixture (the audit's D7 in A minor is V7/VII, not a Dorian IV7);
    a plain major triad is checked for mixture first (A major in A minor is
    the Picardy I before it is V/iv).
    """
    parsed = parse_chord(chord)
    if parsed is None:
        return None
    root_pc, quality = parsed
    norm = _norm_mode(mode)
    numeral = roman_numeral(chord, tonic_pc, norm)
    if numeral is not None and "/" in numeral:
        return SECONDARY
    if is_diatonic(chord, tonic_pc, norm):
        return DIATONIC
    offset = (root_pc - tonic_pc) % 12
    rel = {(pc - tonic_pc) % 12 for pc in _chord_pcs(root_pc, quality)}
    borrowed = any(rel <= set(scale) for scale in _BORROW_SOURCES[norm])
    if quality != "" and _is_secondary(offset, quality, norm):
        return SECONDARY
    if borrowed:
        return BORROWED
    if _is_secondary(offset, quality, norm):
        return SECONDARY
    return CHROMATIC
