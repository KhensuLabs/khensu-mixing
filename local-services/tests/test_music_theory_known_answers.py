"""Music-theory audit 2026-11-06 (THY-01..14, DSP-12 sections, AID-09, UIX-11
server half): regression tests on inputs whose musical answer is known."""
from __future__ import annotations

import math

import numpy as np
import pytest
import soundfile as sf

from khensu_services.analysis import _harmony as hm
from khensu_services.analysis import _note_names as nn

SR = 22050


# --- THY-01 vocal harmony: raised minor degrees, chromatic major notes --------

@pytest.mark.parametrize("note, tonic, mode, interval, direction, expected", [
    (68, 9, "minor", "fifth", "above", 74),   # G#4 in A minor -> D5 (harmonic minor), was D#5
    (68, 9, "minor", "third", "above", 71),   # G#4 -> B4
    (68, 9, "minor", "fifth", "below", 60),   # G#4 -> C4 (III+ of harmonic minor), was C#4
    (68, 9, "minor", "third", "below", 64),   # G#4 -> E4
    (66, 9, "minor", "third", "above", 69),   # F#4 (raised 6th) -> A4 (melodic minor), was A#4
    (61, 9, "minor", "third", "above", 64),   # C#4 (Picardy third) -> E4
    (66, 0, "major", "third", "above", 69),   # F#4 in C (#4) -> A4, was A#4
    (66, 0, "major", "fifth", "above", 72),   # F#4 -> C5 (the V/V chord's seventh)
    (63, 0, "major", "third", "above", 67),   # blue Eb4 in C -> G4
    (63, 0, "major", "fifth", "above", 70),   # Eb4 -> Bb4 (parallel minor)
    (68, 0, "major", "fifth", "above", 75),   # Ab4 (b6) -> Eb5, not E5
    # Diatonic notes unchanged:
    (71, 9, "minor", "third", "above", 74),   # B4 -> D5
    (64, 0, "major", "third", "above", 67),   # E4 -> G4
    (67, 9, "minor", "fifth", "above", 74),   # natural G4 in A minor -> D5
])
def test_vocal_harmony_minor_and_chromatic_degrees(note, tonic, mode, interval, direction, expected):
    from khensu_services.analysis.vocal_harmony import diatonic_harmony_midi

    assert diatonic_harmony_midi(note, tonic, mode, interval, direction) == expected


def test_vocal_harmony_never_leaves_harmonic_minor_on_the_leading_tone():
    from khensu_services.analysis.vocal_harmony import diatonic_harmony_midi

    harmonic = {(9 + d) % 12 for d in (0, 2, 3, 5, 7, 8, 11)}
    for interval in ("third", "fifth"):
        for direction in ("above", "below"):
            out = diatonic_harmony_midi(68, 9, "minor", interval, direction)
            assert out % 12 in harmonic


# --- THY-02 comping pitch score -------------------------------------------------

def _tone(freq_hz: float, sec: float = 1.5, vibrato_cents: float = 0.0) -> np.ndarray:
    t = np.arange(int(sec * SR)) / SR
    ratio = 2.0 ** ((vibrato_cents / 1200.0) * np.sin(2 * np.pi * 5.5 * t))
    phase = np.cumsum(freq_hz * ratio) / SR
    return (0.3 * np.sin(2 * np.pi * phase)).astype(np.float32)


def _cents(hz: float, cents: float) -> float:
    return hz * 2.0 ** (cents / 1200.0)


def test_comping_pitch_score_measures_intonation_not_range():
    from khensu_services.analysis.comping import phrase_intonation, pitch_score_from

    c4, e4, g4 = 261.63, 329.63, 392.0
    in_tune = np.concatenate([_tone(c4, 0.6), _tone(e4, 0.6), _tone(g4, 0.6)])
    sour = np.concatenate([_tone(_cents(f, -35.0), 0.6) for f in (c4, e4, g4)])
    flat_steady = _tone(_cents(220.0, -45.0))
    vibrato = _tone(220.0, vibrato_cents=60.0)

    s_tune = pitch_score_from(phrase_intonation(in_tune, SR))
    s_sour = pitch_score_from(phrase_intonation(sour, SR))
    s_flat = pitch_score_from(phrase_intonation(flat_steady, SR))
    s_vib = pitch_score_from(phrase_intonation(vibrato, SR))
    assert s_tune > 0.85                       # was 0.00 (melodic range)
    assert s_sour < 0.5                        # was 0.00 too - now clearly worse
    assert s_flat < 0.25                       # was 1.00 (perfect)
    assert s_vib > 0.6                         # natural vibrato is not penalised much
    assert s_tune > s_vib > s_sour > s_flat


def test_comping_reference_hz_moves_the_grid():
    from khensu_services.analysis.comping import phrase_intonation

    a432 = _tone(432.0)
    off = phrase_intonation(a432, SR)
    on = phrase_intonation(a432, SR, reference_hz=432.0)
    assert off["mean_abs_cents"] > 25.0
    assert on["mean_abs_cents"] < 5.0


# --- THY-03 duration-weighted key from notes ------------------------------------

def test_key_from_notes_weights_by_duration_times_amplitude():
    from khensu_services.analysis._key import estimate_key_from_notes

    notes, t = [], 0.0
    for p in [60, 64, 67] * 4:                       # long, soft C/E/G
        notes.append({"start_sec": t, "end_sec": t + 2.0, "midi_pitch": p, "velocity": 0.3})
        t += 2.0
    for p in [66, 61, 68] * 6:                       # short, loud passing notes
        notes.append({"start_sec": t, "end_sec": t + 0.1, "midi_pitch": p, "velocity": 1.0})
        t += 0.1
    details = estimate_key_from_notes(notes)
    assert details["key"] == "C major"


def test_midi_variation_uses_the_shared_key_helper():
    from khensu_services.analysis.midi_variation import _estimate_note_key

    notes = [{"start_sec": i * 1.0, "end_sec": i * 1.0 + 0.9, "midi": p, "velocity": 90}
             for i, p in enumerate([57, 60, 64, 57, 62, 65, 64, 68, 69])]
    assert _estimate_note_key(notes) == (9, "minor")


# --- THY-04 chroma fallback dominance test ---------------------------------------

def test_chroma_fallback_skips_chords_and_keeps_single_notes():
    from khensu_services.analysis.advanced_analyzer import _chroma_fallback_pitch_analysis

    t = np.arange(int(2.0 * SR)) / SR
    chord = sum(np.sin(2 * np.pi * f * t) for f in (261.63, 329.63, 392.0)) / 3   # C major triad
    _k, _c, notes = _chroma_fallback_pitch_analysis(chord.astype(np.float32), SR)
    assert notes == []                               # a chord is not a "note"
    single = np.sin(2 * np.pi * 440.0 * t).astype(np.float32)
    _k, _c, notes = _chroma_fallback_pitch_analysis(single, SR)
    assert notes and {n["midi_pitch"] % 12 for n in notes} == {9}


# --- THY-05 / THY-10 chord function categories ------------------------------------

@pytest.mark.parametrize("chord, tonic, mode, function", [
    ("C", 0, "major", "diatonic"), ("G7", 0, "major", "diatonic"),
    ("D7", 0, "major", "secondary"), ("E7", 0, "major", "secondary"),
    ("F#dim", 0, "major", "secondary"), ("F#dim7", 0, "major", "secondary"),
    ("D", 0, "major", "secondary"), ("A", 0, "major", "secondary"),
    ("Bb", 0, "major", "borrowed"), ("Ab", 0, "major", "borrowed"),
    ("Fm", 0, "major", "borrowed"), ("Eb", 0, "major", "borrowed"),
    ("Bdim7", 0, "major", "borrowed"),
    ("F#", 0, "major", "chromatic"), ("Db", 0, "major", "chromatic"),
    ("E7", 9, "minor", "diatonic"), ("G#dim", 9, "minor", "diatonic"),
    ("B7", 9, "minor", "secondary"), ("D7", 9, "minor", "secondary"),
    ("D", 9, "minor", "borrowed"), ("A", 9, "minor", "borrowed"),
    ("N", 0, "major", None), ("Xq", 0, "major", None),
])
def test_chord_function(chord, tonic, mode, function):
    assert hm.chord_function(chord, tonic, mode) == function


def test_secondary_dominant_of_subtonic_in_minor():
    assert hm.roman_numeral("D7", 9, "minor") == "V7/VII"     # was "IV7"


def test_chords_endpoint_function_and_new_templates():
    from khensu_services.analysis.performance_analyzer import detect_chord_sequence

    def chord(*pcs):
        v = np.zeros(12)
        v[list(pcs)] = 1.0
        return v

    c, f, g, d7 = chord(0, 4, 7), chord(5, 9, 0), chord(7, 11, 2), chord(2, 6, 9, 0)
    bdim7 = chord(11, 2, 5, 8)
    caug = chord(0, 4, 8)
    seq = [c] * 6 + [f] * 2 + [d7] * 2 + [g] * 2 + [c] * 4 + [caug] * 2 + [bdim7] * 2 + [c] * 4
    chroma = np.repeat(np.stack(seq, axis=1), 22, axis=1)
    res = detect_chord_sequence(chroma, 22050, 512, segment_sec=0.5)
    assert res["key"] == "C" and res["mode"] == "major"
    by_chord = {e["chord"]: e for e in res["chords"] if e["chord"] != "N"}
    assert by_chord["D7"]["function"] == "secondary" and by_chord["D7"]["roman"] == "V7/V"
    assert by_chord["C"]["function"] == "diatonic"
    assert "Caug" in by_chord
    assert "Bdim7" in by_chord                      # leading-tone chord of the next C
    assert by_chord["Bdim7"]["function"] == "borrowed"   # viio7 from C harmonic minor


def test_harmony_timeline_agrees_with_chords_route_and_reads_noise_as_no_chord(tmp_path):
    from khensu_services.analysis.performance_analyzer import analyze_harmony_timeline

    rng = np.random.default_rng(0)
    noise = (0.2 * rng.standard_normal(SR * 6)).astype(np.float32)
    path = tmp_path / "noise.wav"
    sf.write(str(path), noise, SR)
    out = analyze_harmony_timeline(str(path))
    # The old triad-only matcher labelled every noise second with a chord.
    assert len(out["progression"]) <= 2


# --- THY-07 single-accidental fallback in flat keys -----------------------------

@pytest.mark.parametrize("pc, tonic, expected", [
    (2, 1, "D"),    # Db major: was "Ebb"
    (9, 1, "A"),    # was "Bbb"
    (4, 1, "E"),    # was "Fb"
    (11, 1, "B"),   # was "Cb" (b7 of Db)
    (7, 6, "G"),    # Gb major: was "Abb"
    (11, 6, "Cb"),  # diatonic 4th of Gb major keeps its spelling
    (11, 10, "B"),  # Bb major b2: was "Cb"
    (10, 0, "Bb"),  # C major bVII unchanged
    (7, 8, "F##"),  # G# minor leading tone still theory-required
])
def test_flat_keys_avoid_double_flats(pc, tonic, expected):
    mode = "minor" if (pc, tonic) == (7, 8) else "major"
    assert nn.spell_degree(pc, tonic, mode) == expected


def test_a_major_chord_in_db_reads_a():
    assert nn.spell_degree(9, 1, "major") + "" == "A"
    assert nn.midi_name(62, 1, "major") == "D4"


# --- THY-09 one quarter-note tempo convention ------------------------------------

def test_quarter_note_bpm():
    from khensu_services.analysis._shared_analysis import quarter_note_bpm

    assert quarter_note_bpm({"time_signature": "6/8", "beats_per_bar": 6}, 180.0) == 90.0
    assert quarter_note_bpm({"time_signature": "6/8", "beats_per_bar": 2}, 60.0) == 90.0
    assert quarter_note_bpm({"time_signature": "4/4", "beats_per_bar": 4}, 128.0) == 128.0
    assert quarter_note_bpm("3/4", 90.0) == 90.0
    assert quarter_note_bpm("6/8", 180.0) == 90.0
    assert quarter_note_bpm(None, None) is None
    assert quarter_note_bpm("4/4", 0.0) is None


def test_analyze_response_declares_quarter_note_bpm_and_section_lufs():
    from khensu_services.analysis.models import AnalyzeResponse, SectionMarker

    assert "quarter_note_bpm" in AnalyzeResponse.model_fields
    assert SectionMarker(start_sec=0.0, label="A").lufs_integrated is None


# --- MIDI downbeat alignment ------------------------------------------------------

def test_midi_bar_one_on_first_downbeat(tmp_path):
    import mido

    from khensu_services.common.midi_io import MidiNote, downbeat_offset_sec, write_midi_notes

    notes = [MidiNote(0.5, 1.0, 60, 90), MidiNote(1.0, 1.5, 62, 90)]
    # 120 BPM 4/4: bar = 2 s. Downbeat at 0.5 s, nothing before it -> tick 0.
    assert downbeat_offset_sec(notes, 120.0, "4/4", 0.5) == pytest.approx(-0.5)
    out = tmp_path / "a.mid"
    offset = write_midi_notes(notes, str(out), 120.0, time_signature="4/4", first_downbeat_sec=0.5)
    assert offset == pytest.approx(-0.5)
    first_on = next(m for m in mido.MidiFile(str(out)).tracks[0] if m.type == "note_on")
    ticks = sum(m.time for m in mido.MidiFile(str(out)).tracks[0][:mido.MidiFile(str(out)).tracks[0].index(first_on) + 1])
    assert ticks == 0
    # A pickup note before the downbeat gets one bar of lead-in instead of being cut.
    pickup = [MidiNote(0.2, 0.4, 59, 90)] + notes
    assert downbeat_offset_sec(pickup, 120.0, "4/4", 0.5) == pytest.approx(1.5)
    # No downbeat -> unchanged.
    assert downbeat_offset_sec(notes, 120.0, "4/4", None) == 0.0


# --- THY-12 tuning caveat + vibrato hysteresis ------------------------------------

def test_tuning_reference_reports_semitone_ambiguity(tmp_path):
    from khensu_services.analysis.tuning_reference import analyze_tuning_reference

    path = tmp_path / "a415.wav"
    sf.write(str(path), _tone(415.3, 4.0), SR)
    out = analyze_tuning_reference(str(path))
    assert out["caveat"] and "semitone" in out["caveat"]
    lo, hi = out["semitone_alternatives_hz"]
    assert lo < out["reference_hz"] < hi


def test_wide_vibrato_is_one_note():
    from khensu_services.analysis.pitch_accuracy import analyze_pitch_accuracy

    y = _tone(261.63, 2.0, vibrato_cents=80.0)
    out = analyze_pitch_accuracy(y, SR)
    assert len(out["notes"]) == 1 and out["notes"][0]["note_midi"] == 60


# --- THY-14 / AID-09 -----------------------------------------------------------

def test_hz_to_midi_scalar_sentinel_is_nan():
    assert math.isnan(nn.hz_to_midi(0.0))
    assert nn.hz_to_midi(440.0) == pytest.approx(69.0)


def test_mood_labels_and_tagger_path():
    from khensu_services.analysis import mood_energy as me
    from khensu_services.common.model_paths import models_dir

    assert me.estimate_mood("A minor", 80.0, 128.0) == "intense"      # was "aggressive"
    assert me.estimate_mood("C major", 80.0, 128.0) == "euphoric"
    if (models_dir() / "mood_tagger.json").is_file():
        assert me._mood_tagger_model_path() == models_dir() / "mood_tagger.json"


def test_memorability_bpm_prior_comment_matches_maths():
    assert math.exp(-0.5 * ((170.0 - 120.0) / 40.0) ** 2) == pytest.approx(0.458, abs=1e-3)


# --- UIX-11 server half: caller-chosen render paths --------------------------------

def _take(tmp_path, name):
    t = np.arange(int(2.0 * SR)) / SR
    y = np.concatenate([np.zeros(SR // 2), 0.3 * np.sin(2 * np.pi * 220 * t), np.zeros(SR)])
    p = tmp_path / name
    sf.write(str(p), y.astype(np.float32), SR)
    return str(p)


def test_comping_render_honours_output_path(client, tmp_path):
    a, b = _take(tmp_path, "a.wav"), _take(tmp_path, "b.wav")
    out = tmp_path / "renders" / "comp_0001.wav"
    out.parent.mkdir()
    resp = client.post("/analyze/comping", json={"take_paths": [a, b], "render": True,
                                                  "output_path": str(out)})
    assert resp.status_code == 200, resp.text
    assert resp.json()["rendered_path"] == str(out) and out.is_file()
    alias = client.post("/analyze/comping", json={"take_paths": [a, b], "render": True,
                                                   "output_path": a})
    assert alias.status_code == 409


def test_align_takes_render_honours_output_paths(client, tmp_path):
    ref, take = _take(tmp_path, "ref.wav"), _take(tmp_path, "take.wav")
    out = tmp_path / "take_aligned_0002.wav"
    resp = client.post("/analyze/align_takes", json={"reference_path": ref, "take_paths": [take],
                                                      "output_paths": [str(out)]})
    assert resp.status_code == 200, resp.text
    assert out.is_file()
    bad = client.post("/analyze/align_takes", json={"reference_path": ref, "take_paths": [take],
                                                     "output_paths": [str(out), str(out)]})
    assert bad.status_code == 422
