"""Chord/note spelling follows the scale degree the Roman numeral implies
(audit H4) + related-key / half-diminished lows."""
from __future__ import annotations

import numpy as np
import pytest

from khensu_services.analysis import _harmony
from khensu_services.analysis._note_names import midi_name, spell_degree


@pytest.mark.parametrize("pc, tonic, mode, expected", [
    (10, 0, "major", "Bb"),   # bVII in C
    (8, 0, "major", "Ab"),    # bVI in C
    (6, 7, "minor", "F#"),    # #vii in G minor
    (1, 2, "minor", "C#"),    # #vii in D minor
    (6, 0, "major", "F#"),    # #iv in C
    (3, 0, "minor", "Eb"),    # iii in C minor
    (10, 5, "major", "Bb"),   # IV in F
    (4, 9, "minor", "E"),
    (7, 8, "minor", "F##"),   # leading tone of G# minor (theoretically required)
])
def test_spell_degree(pc, tonic, mode, expected):
    assert spell_degree(pc, tonic, mode) == expected


def test_spelled_root_agrees_with_numeral():
    for tonic in range(12):
        for mode in ("major", "minor"):
            for root in range(12):
                label = spell_degree(root, tonic, mode)
                numeral = _harmony.roman_numeral(label, tonic, mode)
                acc = numeral[0] if numeral[0] in "b#" else ""
                letter_acc = label[1:]
                # A flat numeral never comes with a sharp-spelled root and v.v.
                if acc == "b":
                    assert "#" not in letter_acc, (tonic, mode, root, label, numeral)
                if acc == "#":
                    assert "b" not in letter_acc, (tonic, mode, root, label, numeral)


def test_midi_name_keeps_leading_tone_sharp_in_minor():
    assert midi_name(61, 2, "minor") == "C#4"       # was Db4
    assert midi_name(60, 0, "major") == "C4"
    assert midi_name(59, 6, "major") == "Cb4"       # octave follows the letter
    assert midi_name(61) == "C#4"                    # keyless unchanged


def test_detect_chord_sequence_spells_borrowed_chords_from_degree():
    from khensu_services.analysis.performance_analyzer import detect_chord_sequence

    def triad(root, minor=False):
        v = np.zeros(12)
        for iv in (0, 3 if minor else 4, 7):
            v[(root + iv) % 12] = 1.0
        return v

    # C major: I - bVII - bVI - I, with C-major-heavy context so the key is C.
    seq = [triad(0)] * 6 + [triad(5)] * 2 + [triad(7)] * 2 + [triad(10)] * 2 + [triad(8)] * 2 + [triad(0)] * 4
    chroma = np.repeat(np.stack(seq, axis=1), 22, axis=1)
    res = detect_chord_sequence(chroma, 22050, 512, segment_sec=0.5)
    assert res["key"] == "C" and res["mode"] == "major"
    pairs = {(c["chord"], c["roman"]) for c in res["chords"] if c["chord"] != "N"}
    assert ("Bb", "bVII") in pairs
    assert ("Ab", "bVI") in pairs
    assert not any(c["chord"].startswith(("A#", "G#")) for c in res["chords"])


def test_half_diminished_numeral_is_m7b5():
    assert _harmony.roman_numeral("Bm7b5", 0, "major") == "viim7b5"
    assert _harmony.roman_numeral("Bdim7", 0, "major") == "viio7"


def test_related_keys_keep_source_spelling_on_enharmonic_ties():
    assert _harmony.parallel_key(6, "minor") == "F# major"           # not Gb major
    assert _harmony.parallel_key(1, "major") == "C# minor"           # Db minor = 8 flats
    assert _harmony.relative_key(6, "major", prefer_flats=False) == "D# minor"
    assert _harmony.relative_key(6, "major", prefer_flats=True) == "Eb minor"
    assert _harmony.parallel_key(3, "major") == "Eb minor"


def test_advanced_key_relations_follow_key_string():
    from khensu_services.analysis.advanced_analyzer import _key_relations

    assert _key_relations("F# major")["relative_key"] == "D# minor"
    assert _key_relations("Gb major")["relative_key"] == "Eb minor"
