"""Music-theory audit fixes (2026-10-10): key spelling, harmony tracking,
groove subdivision, beat-following quantize grid, chord labelling, tuning
reference, downbeat phase, MIDI variation/extraction, tempo-octave folding."""
from __future__ import annotations

import mido
import numpy as np
import pytest
import soundfile as sf

from khensu_services.analysis import _note_names as nn

SR = 22050


def _write(tmp_path, name, y, sr=SR):
    p = tmp_path / name
    sf.write(str(p), np.asarray(y, dtype=np.float32), sr, subtype="FLOAT")
    return str(p)


def _chord_audio(freqs, dur=1.0, sr=SR):
    t = np.arange(int(sr * dur)) / sr
    return 0.3 * sum(sum(np.sin(2 * np.pi * f * h * t) / h for h in (1, 2, 3))
                     for f in freqs) / len(freqs)


def _clicks(times, sec, sr=SR):
    y = np.zeros(int(sec * sr), dtype=np.float32)
    n = int(0.04 * sr)
    t = np.arange(n) / sr
    burst = (0.8 * np.sin(2 * np.pi * 1000.0 * t) * np.exp(-t / 0.008)).astype(np.float32)
    for c in times:
        a = int(round(c * sr))
        y[a:a + n] += burst[: len(y) - a]
    return y


def _tone_sequence(ref_a: float, semis, seg_sec=0.6, sr=SR):
    out = []
    for s in semis:
        f = ref_a * 2 ** (s / 12) / 2
        t = np.arange(int(seg_sec * sr)) / sr
        out.append(0.3 * np.sin(2 * np.pi * f * t))
    return np.concatenate(out)


C_MAJ = (261.63, 329.63, 392.0)
F_MAJ = (349.23, 440.0, 523.25)
G_MAJ = (392.0, 493.88, 587.33)
EB_MAJ = (311.13, 392.0, 466.16)
AB_MAJ = (415.30, 523.25, 622.25)
BB_MAJ = (233.08, 293.66, 349.23)


# --- #7 shared spelling / pitch helpers ------------------------------------

def test_key_aware_spelling_helpers():
    assert nn.key_name(10, "major") == "Bb major"
    assert nn.key_name(7, "minor") == "G minor"        # relative of Bb major
    assert nn.key_name(1, "major") == "Db major"
    assert nn.key_name(6, "major") == "Gb major"
    assert nn.key_name(3, "minor") == "Eb minor"       # relative of Gb major
    assert nn.key_name(11, "major") == "B major"
    assert nn.key_name(1, "minor") == "C# minor"
    assert [nn.pc_name(pc, 10, "major") for pc in (10, 3, 5)] == ["Bb", "Eb", "F"]
    assert nn.midi_name(70, 5, "major") == "Bb4"
    assert nn.midi_name(70) == "A#4"                   # no key -> sharps
    assert nn.parse_pitch_class("Bb") == 10 and nn.parse_pitch_class("E♭") == 3
    assert nn.parse_key("Bb major") == (10, "major")
    assert nn.parse_key("F#m") == (6, "minor")
    assert nn.parse_key("unknown") is None
    with pytest.raises(ValueError):
        nn.parse_pitch_class("H")


def test_circular_cents_statistics():
    # +49 and -49 cents are 2 cents apart on the semitone circle.
    assert abs(nn.circular_mean_cents([49.0, -49.0]) - (-50.0)) < 1e-6 \
        or abs(nn.circular_mean_cents([49.0, -49.0]) - 50.0) < 1e-6
    assert abs(float(nn.circular_cents_diff(49.0, -49.0)) - (-2.0)) < 1e-9
    assert abs(nn.circular_mean_cents([-30.0, -34.0, -32.0]) + 32.0) < 1e-6


# --- #2 harmony timeline key tracking ---------------------------------------

def test_harmony_timeline_fixed_key_loop_has_no_key_changes(tmp_path):
    from khensu_services.analysis.performance_analyzer import analyze_harmony_timeline

    y = np.concatenate([_chord_audio(c) for c in [C_MAJ, F_MAJ, G_MAJ, C_MAJ] * 10])
    out = analyze_harmony_timeline(_write(tmp_path, "loop.wav", y))
    assert out["initial_key"] == {"time_sec": 0.0, "key": "C", "mode": "major"}
    assert out["key_changes"] == []
    assert {p["chord"] for p in out["progression"]} == {"C", "F", "G"}


def test_harmony_timeline_reports_sustained_modulation_once(tmp_path):
    from khensu_services.analysis.performance_analyzer import analyze_harmony_timeline

    first = np.concatenate([_chord_audio(c) for c in [C_MAJ, F_MAJ, G_MAJ, C_MAJ] * 10])
    second = np.concatenate([_chord_audio(c) for c in [EB_MAJ, AB_MAJ, BB_MAJ, EB_MAJ] * 10])
    out = analyze_harmony_timeline(_write(tmp_path, "mod.wav", np.concatenate([first, second])))
    assert len(out["key_changes"]) == 1
    change = out["key_changes"][0]
    assert (change["key"], change["mode"]) == ("Eb", "major")
    assert 36.0 <= change["time_sec"] <= 52.0
    # Chords after the change are spelled for Eb major (Ab / Bb, not G# / A#).
    late = {p["chord"] for p in out["progression"] if p["time_sec"] > 60.0}
    assert late == {"Eb", "Ab", "Bb"}


# --- #3 groove subdivision ---------------------------------------------------

def test_groove_quantised_sixteenths_have_near_zero_spread(tmp_path):
    from khensu_services.analysis.performance_analyzer import analyze_groove_timing

    times = np.arange(0.0, 8.0, 0.125)                 # 16ths at 120 BPM
    y = np.zeros(int(SR * 8.3))
    for i, t in enumerate(times):
        a = int(round(t * SR))
        y[a:a + 80] += (1.0 if i % 4 == 0 else 0.6) * np.hanning(80)
    out = analyze_groove_timing(_write(tmp_path, "sixteenths.wav", y))
    assert out["grid_subdivision"] == "16th"
    assert abs(out["bpm"] - 120.0) < 3.0
    assert out["timing_std_ms"] < 5.0                  # was ~+-60 ms against the beat grid
    assert out["pocket"] == "on_grid"


# --- #4 beat-following quantize grid -----------------------------------------

def test_quantize_without_bpm_follows_tracked_tempo_drift(tmp_path):
    from khensu_services.analysis.take_alignment import align_takes

    periods = np.linspace(0.5, 0.42, 20)               # gradual accelerando
    beats = 0.5 + np.concatenate([[0.0], np.cumsum(periods)])
    sec = float(beats[-1]) + 1.0
    ref = _write(tmp_path, "ref.wav", _clicks(beats, sec))
    mids = (beats[:-1] + beats[1:]) / 2
    take_times = np.sort(np.concatenate([beats[1:-1], mids[1:-1]])) + 0.015
    take = _write(tmp_path, "take.wav", _clicks(take_times, sec))

    res = align_takes(ref, [take], mode="quantize", grid_division=8, render=False)
    tk = res["takes"][0]
    assert abs(tk["mean_abs_offset_ms_before"] - 15.0) < 4.0
    assert tk["mean_abs_offset_ms_after"] < 1.0
    assert all(abs(s["shift_ms"] + 15.0) < 4.0 for s in tk["segments"])

    # A constant-period grid at the same average tempo cannot follow the drift.
    const = align_takes(ref, [take], mode="quantize", bpm=res["bpm"], grid_division=8,
                        render=False)["takes"][0]
    assert any(abs(s["shift_ms"] + 15.0) > 10.0 for s in const["segments"])


# --- #5 chord labelling ---------------------------------------------------------

def _pcs(*notes):
    v = np.zeros(12)
    v[list(notes)] = 1.0
    return v


def _chroma(segs, frames=21):
    return np.concatenate([np.tile(s[:, None], (1, frames)) for s in segs], axis=1)


def test_chord_sequence_templates_no_chord_and_smoothing():
    from khensu_services.analysis.performance_analyzer import detect_chord_sequence

    segs = [_pcs(0, 4, 7, 10)] * 2 + [np.zeros(12)] * 2 + [np.ones(12)] * 2 \
        + [_pcs(2, 5, 9)] * 2 + [_pcs(0, 5, 7)] * 2 + [_pcs(11, 2, 5)] * 2
    labels = [c["chord"] for c in detect_chord_sequence(_chroma(segs), SR, 512)["chords"]]
    assert labels == ["C7", "C7", "N", "N", "N", "N", "Dm", "Dm",
                      "Csus4", "Csus4", "Bdim", "Bdim"]

    # One ambiguous segment inside a held C chord is smoothed away.
    amb = np.zeros(12)
    amb[[0, 4, 7, 9]] = [1.0, 1.0, 0.5, 0.6]
    out = detect_chord_sequence(_chroma([_pcs(0, 4, 7)] * 5 + [amb] + [_pcs(0, 4, 7)] * 5), SR, 512)
    assert {c["chord"] for c in out["chords"]} == {"C"}


def test_chord_sequence_key_and_spelling_in_bb_major():
    from khensu_services.analysis.performance_analyzer import detect_chord_sequence

    bb, eb, f = _pcs(10, 2, 5), _pcs(3, 7, 10), _pcs(5, 9, 0)
    out = detect_chord_sequence(_chroma([bb, bb, eb, eb, f, f, bb, bb] * 3), SR, 512)
    assert (out["key"], out["mode"]) == ("Bb", "major")
    assert {c["chord"] for c in out["chords"]} == {"Bb", "Eb", "F"}
    silent = detect_chord_sequence(np.zeros((12, 50)), SR, 512)
    assert (silent["key"], silent["mode"], silent["key_confidence"]) == ("unknown", "", 0.0)
    assert {c["chord"] for c in silent["chords"]} == {"N"}


def test_chords_endpoint_shape_and_flat_spelling(client, tmp_path):
    y = np.concatenate([_chord_audio(c) for c in [BB_MAJ, EB_MAJ, F_MAJ, BB_MAJ] * 3])
    resp = client.post("/analyze/chords", json={"file_path": _write(tmp_path, "bb.wav", y)})
    assert resp.status_code == 200, resp.text
    body = resp.json()
    assert set(body) == {"chords", "key", "mode", "key_confidence", "camelot",
                         "circle_of_fifths_index", "relative_key", "parallel_key"}
    assert (body["camelot"], body["relative_key"]) == ("6B", "G minor")
    assert (body["key"], body["mode"]) == ("Bb", "major")
    chords = {c["chord"] for c in body["chords"]}
    assert {"Bb", "Eb", "F"} <= chords
    assert not chords & {"A#", "D#"}


# --- #6 tuning reference -------------------------------------------------------

def test_pitch_accuracy_reports_detected_reference_but_measures_requested_grid():
    # Audit H3 (2026-10-11): intonation is measured against A=440 by default
    # (a uniformly-off take must not read as in tune); the take's own
    # reference is still detected/reported, and requesting it as the grid
    # measures the take against itself.
    from khensu_services.analysis.pitch_accuracy import analyze_pitch_accuracy

    y = _tone_sequence(432.0, [0, 2, 4, 5, 7, 5, 4, 2])
    out = analyze_pitch_accuracy(y, SR)
    assert abs(out["reference_hz"] - 432.0) < 1.5      # detected, still reported
    assert out["measurement_reference_hz"] == 440.0
    assert out["reference_offset_warning"] is True
    assert out["mean_abs_cents"] > 25.0                # ~32 cents flat of A=440
    own = analyze_pitch_accuracy(y, SR, reference_hz=432.0)
    assert own["mean_abs_cents"] < 6.0
    assert own["in_tune_pct"] == 100.0


def test_pitch_accuracy_hysteresis_keeps_wide_vibrato_as_one_note():
    from khensu_services.analysis.pitch_accuracy import analyze_pitch_accuracy

    t = np.arange(int(2.0 * SR)) / SR
    f = 220.0 * 2 ** (60.0 * np.sin(2 * np.pi * 5.5 * t) / 1200.0)   # +-60 cents
    y = 0.3 * np.sin(2 * np.pi * np.cumsum(f) / SR)
    out = analyze_pitch_accuracy(y, SR)
    assert [n["note_name"] for n in out["notes"]] == ["A3"]
    assert abs(out["notes"][0]["mean_cents_deviation"]) < 8.0
    assert abs(out["reference_hz"] - 440.0) < 3.0


def test_pitch_curve_targets_are_on_the_takes_reference(tmp_path):
    from khensu_services.analysis.pitch_editor import analyze_pitch_curve

    path = _write(tmp_path, "v432.wav", _tone_sequence(432.0, [0, 2, 4, 5, 7]))
    default = analyze_pitch_curve(path)                 # A=440 grid by default (H3)
    assert default["reference_hz"] == 440.0
    assert abs(default["detected_reference_hz"] - 432.0) < 1.5
    assert default["reference_offset_warning"] is True
    out = analyze_pitch_curve(path, reference_hz=432.0)  # request the take's own grid
    assert abs(out["reference_hz"] - 432.0) < 1.5
    assert out["notes"]
    # target_midi - midi is the correction; on its own reference the take is in tune.
    assert all(abs(n["target_midi"] - n["midi"]) < 0.08 for n in out["notes"])


def test_tuning_drift_is_circular_and_includes_final_window(tmp_path, monkeypatch):
    import librosa

    from khensu_services.analysis.tuning_reference import analyze_tuning_reference

    values = iter([0.0, 0.49, -0.49, 0.48])            # global, then 3 windows
    monkeypatch.setattr(librosa, "estimate_tuning", lambda **_kw: next(values))
    path = _write(tmp_path, "six.wav", np.zeros(SR * 6))
    out = analyze_tuning_reference(path, window_sec=2.0)
    assert len(out["drift"]) == 3                      # the final full window counts
    assert out["drift_range_cents"] <= 3.0             # not 98
    assert out["drifting"] is False


def test_micro_pitch_grid_lock_is_measured_against_own_reference(tmp_path):
    from khensu_services.analysis.artifacts.deep_tier import VocalMicroPitchDetector

    path = _write(tmp_path, "locked432.wav", _tone_sequence(432.0, [0, 2, 4, 5, 7, 5, 4, 2]))
    findings = VocalMicroPitchDetector().scan_stem(path)
    assert [f.kind for f in findings] == ["vocal_micro_pitch"]


# --- #8 downbeat phase ------------------------------------------------------------

def test_downbeat_phase_follows_the_kick_and_quick_grid_uses_it():
    from khensu_services.analysis._shared_analysis import estimate_downbeat_phase

    beat_times = 0.25 + 0.5 * np.arange(33)
    y = np.zeros(int((beat_times[-1] + 1.0) * SR))
    t = np.arange(int(0.08 * SR)) / SR
    hat = 0.3 * np.random.default_rng(0).standard_normal(len(t)) * np.exp(-t / 0.01)
    kick = 0.9 * np.sin(2 * np.pi * 55 * t) * np.exp(-t / 0.05)
    for i, b in enumerate(beat_times):
        a = int(b * SR)
        y[a:a + len(t)] += hat + (kick if i % 4 == 1 else 0.0)   # one pickup beat
    assert estimate_downbeat_phase(y, SR, beat_times.tolist()) == 1
    # Too few beats / silence: historical phase 0.
    assert estimate_downbeat_phase(np.zeros(SR), SR, [0.0, 0.5, 1.0]) == 0


# --- #9 MIDI variation ---------------------------------------------------------------

def _fake_notes(monkeypatch, notes):
    monkeypatch.setattr("khensu_services.analysis.midi_extract.extract_midi", lambda *_a: {
        "kind": "pitched", "extraction_backend": "librosa_pyin_monophonic", "notes": notes})


def test_midi_variation_is_diatonic_seeded_and_writes_detected_tempo(tmp_path, monkeypatch):
    from khensu_services.analysis.midi_variation import apply_midi_variation

    c_major = [60, 62, 64, 65, 67, 69, 71, 72, 64, 67, 60, 65]
    notes = [{"start_sec": 0.5 * i, "end_sec": 0.5 * i + 0.4, "midi": m,
              "velocity": 90, "note": nn.midi_name(m)} for i, m in enumerate(c_major)]
    _fake_notes(monkeypatch, notes)
    beats = np.arange(0.0, 9.0, 0.6)                    # 100 BPM click track
    src = _write(tmp_path, "src.wav", _clicks(beats, 9.5))
    kw = dict(stem_type="lead", timing_variance=0.0, velocity_variance=0.0,
              pitch_variation_prob=1.0)
    out = apply_midi_variation(src, output_path=str(tmp_path / "a.mid"), **kw)
    again = apply_midi_variation(src, output_path=str(tmp_path / "b.mid"), **kw)
    assert out["events"] == again["events"]             # stable seed (crc32, not hash())
    assert out["key"] == "C major"
    scale = {0, 2, 4, 5, 7, 9, 11}
    for ev, src_note in zip(out["events"], notes):
        assert ev["midi"] % 12 in scale                  # in key
        assert 1 <= abs(ev["midi"] - src_note["midi"]) <= 2
        assert ev["note"] == nn.midi_name(ev["midi"])
    assert abs(out["bpm"] - 100.0) < 4.0
    tempo = next(m.tempo for m in mido.MidiFile(str(tmp_path / "a.mid")).tracks[0]
                 if m.type == "set_tempo")
    assert abs(mido.tempo2bpm(tempo) - out["bpm"]) < 0.01


def test_midi_variation_note_off_sorts_before_same_time_note_on(tmp_path, monkeypatch):
    from khensu_services.analysis.midi_variation import apply_midi_variation

    notes = [{"start_sec": 0.0, "end_sec": 0.5, "midi": 60, "velocity": 90, "note": "C4"},
             {"start_sec": 0.5, "end_sec": 1.0, "midi": 60, "velocity": 90, "note": "C4"}]
    _fake_notes(monkeypatch, notes)
    src = _write(tmp_path, "src.wav", _clicks([0.0, 0.5], 1.5))
    out_path = tmp_path / "rep.mid"
    apply_midi_variation(src, stem_type="lead", timing_variance=0.0, velocity_variance=0.0,
                         pitch_variation_prob=0.0, output_path=str(out_path))
    msgs = [m for m in mido.MidiFile(str(out_path)).tracks[0] if m.type in ("note_on", "note_off")]
    assert [m.type for m in msgs] == ["note_on", "note_off", "note_on", "note_off"]


# --- #10 pyin MIDI fallback hysteresis -----------------------------------------------

def test_pyin_midi_fallback_keeps_vibrato_as_one_note():
    from khensu_services.analysis.midi_extract import extract_pitched

    t = np.arange(int(2.0 * SR)) / SR
    f = 220.0 * 2 ** (60.0 * np.sin(2 * np.pi * 5.5 * t) / 1200.0)
    y = (0.3 * np.sin(2 * np.pi * np.cumsum(f) / SR)).astype(np.float32)
    out = extract_pitched(y, SR)
    assert [n["midi"] for n in out["notes"]] == [57]


# --- #12 half/double-time folding --------------------------------------------------

def test_genre_blend_folds_half_and_double_time(monkeypatch):
    from khensu_services.analysis import genre_detector

    y = 0.1 * np.random.default_rng(1).standard_normal(SR * 2)
    centroid = 2000.0
    monkeypatch.setattr(genre_detector, "_anchor_cache", [
        ("trap", "hip-hop", 140.0, centroid, 0.5, 10.0),
        ("mid-tempo", "pop", 100.0, centroid, 0.5, 10.0),
    ])
    feats = {"low_end_weight": 0.5, "crest_factor_db": 10.0}
    blend = genre_detector.estimate_genre_blend(y, SR, 70.0, measured_features=feats)
    assert blend[0]["genre"] == "trap"                   # 70 BPM half-time == 140
