"""Music-theory additions (2026-10-10): meter / time-signature detection,
octave-folded genre tempo matching + genre_targets.json tempo convention,
key relationships (Camelot, circle of fifths, relative/parallel keys) and
Roman-numeral chord labels, and the basic-pitch worker's shared note table."""
from __future__ import annotations

import json
from pathlib import Path

import numpy as np
import pytest

from khensu_services.analysis import _harmony as hm
from khensu_services.analysis import _note_names as nn
from khensu_services.analysis import _shared_analysis as sa

SR = 22050
_T = np.arange(int(0.08 * SR)) / SR
_HAT = 0.3 * np.random.default_rng(0).standard_normal(len(_T)) * np.exp(-_T / 0.01)
_KICK = 0.9 * np.sin(2 * np.pi * 55 * _T) * np.exp(-_T / 0.05)


def _groove(beats_per_bar, n_bars=16, ibi=0.5, pickup=0, accent=None, sub=()):
    """Hat on every beat, kick on the bar's accented beats, optional quieter
    hats at fractional sub-beat positions. Returns (y, beat_times)."""
    beats = 0.25 + ibi * np.arange(n_bars * beats_per_bar + pickup)
    y = np.zeros(int((beats[-1] + 1.0) * SR))
    for i, b in enumerate(beats):
        a = int(b * SR)
        pos = (i - pickup) % beats_per_bar
        gain = accent(pos) if accent else (1.0 if pos == 0 else 0.0)
        y[a:a + len(_T)] += _HAT + gain * _KICK
        for frac in sub:
            s = int((b + frac * ibi) * SR)
            y[s:s + len(_T)] += 0.5 * _HAT
    return y, beats.tolist()


# --- meter ----------------------------------------------------------------------

def test_meter_detects_four_four_kick_pattern():
    y, beats = _groove(4, pickup=2)
    m = sa.estimate_meter(y, SR, beats)
    assert (m["time_signature"], m["beats_per_bar"]) == ("4/4", 4)
    assert m["meter_confidence"] >= 0.5
    assert m["downbeat_phase"] == 2


def test_meter_detects_four_four_backbeat_and_straight_eighths():
    y, beats = _groove(4, accent=lambda p: 1.0 if p == 0 else (0.6 if p == 2 else 0.0),
                       sub=(0.5,))
    m = sa.estimate_meter(y, SR, beats)
    assert m["time_signature"] == "4/4"
    assert m["meter_confidence"] >= 0.5


def test_meter_detects_waltz_and_its_pickup():
    y, beats = _groove(3, pickup=1)
    m = sa.estimate_meter(y, SR, beats)
    assert (m["time_signature"], m["beats_per_bar"]) == ("3/4", 3)
    assert m["meter_confidence"] >= 0.5
    assert m["downbeat_phase"] == 1
    # The generalised phase estimator agrees at 3 beats per bar.
    assert sa.estimate_downbeat_phase(y, SR, beats, 3) == 1


def test_meter_detects_compound_duple_from_ternary_subdivision():
    # Dotted-quarter pulse (80 BPM), each beat split in three eighths.
    y, beats = _groove(2, ibi=0.75, sub=(1 / 3, 2 / 3))
    m = sa.estimate_meter(y, SR, beats)
    assert (m["time_signature"], m["beats_per_bar"]) == ("6/8", 2)


_SNARE = 0.7 * np.diff(np.random.default_rng(1).standard_normal(len(_T) + 1)) * np.exp(-_T / 0.03)
_CHORDS = [(261.63, 329.63, 392.0), (220.0, 261.63, 329.63), (174.61, 220.0, 261.63), (196.0, 246.94, 293.66)]


def _bar_groove(bpb, ibi, n_bars=16, kick_at=(0,), snare_at=(), chords=False, pickup=0):
    """Hat every tracked beat, kick / bright snare on the given bar positions,
    and optionally one sustained triad per bar (harmonic rhythm = the bar)."""
    beats = 0.25 + ibi * np.arange(n_bars * bpb + pickup)
    y = np.zeros(int((beats[-1] + 1.0) * SR))
    for i, b in enumerate(beats):
        a = int(b * SR)
        pos = (i - pickup) % bpb
        y[a:a + len(_T)] += _HAT + (_KICK if pos in kick_at else 0.0) + (_SNARE if pos in snare_at else 0.0)
    if chords:
        t = np.arange(int(bpb * ibi * SR)) / SR
        for k in range(n_bars):
            a = int((0.25 + (pickup + k * bpb) * ibi) * SR)
            tone = 0.08 * sum(np.sin(2 * np.pi * f * t) for f in _CHORDS[k % 4])
            y[a:a + len(t)] += tone[: len(y) - a]
    return y, beats.tolist()


@pytest.mark.parametrize("bpm,chords,pickup", [(150, True, 0), (180, False, 0), (180, True, 2), (220, False, 0)])
def test_meter_detects_six_eight_tracked_at_the_eighth_note_pulse(bpm, chords, pickup):
    # Kick on eighth 1, backbeat snare on eighth 4: the generic accent curve
    # peaks every 3 beats like a waltz; the kick and the chord change recur
    # only every 6.
    y, beats = _bar_groove(6, 60.0 / bpm, kick_at=(0,), snare_at=(3,), chords=chords, pickup=pickup)
    m = sa.estimate_meter(y, SR, beats, float(bpm))
    assert (m["time_signature"], m["beats_per_bar"]) == ("6/8", 6)
    assert m["downbeat_phase"] == pickup


@pytest.mark.parametrize("bpm,snare_at", [(160, ()), (180, (1, 2))])
def test_fast_waltz_is_not_promoted_to_six_eight(bpm, snare_at):
    # Kick (and the chord) every 3 beats - a waltz, even at an eighth-pulse tempo.
    y, beats = _bar_groove(3, 60.0 / bpm, kick_at=(0,), snare_at=snare_at, chords=True)
    m = sa.estimate_meter(y, SR, beats, float(bpm))
    assert (m["time_signature"], m["beats_per_bar"]) == ("3/4", 3)


def test_meter_falls_back_to_four_four_with_low_confidence():
    # Identical clicks: no accent structure at all.
    y, beats = _groove(4, accent=lambda p: 0.0)
    m = sa.estimate_meter(y, SR, beats)
    assert m["time_signature"] == "4/4" and m["meter_confidence"] <= 0.2
    # Too few beats / silence.
    short = sa.estimate_meter(np.zeros(SR), SR, [0.0, 0.5, 1.0])
    assert short == {"time_signature": "4/4", "beats_per_bar": 4,
                     "meter_confidence": 0.0, "downbeat_phase": 0}


def test_advanced_beats_use_detected_meter_for_downbeats(monkeypatch):
    import librosa

    from khensu_services.analysis import advanced_analyzer as aa
    from khensu_services.analysis import onnx_inference

    y, beats = _groove(3, pickup=1)
    frames = librosa.time_to_frames(np.asarray(beats), sr=SR)
    monkeypatch.setattr(onnx_inference, "predict_key_tempo_onnx", lambda *_a, **_k: None)
    monkeypatch.setattr(librosa.beat, "beat_track",
                        lambda **_k: (np.array([120.0]), frames))
    beat_times, downbeats, _bpm, _conf, meter = aa._librosa_beats(y, SR)
    assert meter["time_signature"] == "3/4"
    assert downbeats == beat_times[1::3]


def test_analyze_response_meter_and_key_fields_are_optional():
    from khensu_services.analysis.models import AnalyzeResponse

    base = dict(mode="quick", bpm=120.0, bpm_confidence=0.9, beat_times=[],
                downbeat_times=None, key="C major", key_confidence=0.8, sections=[],
                pitch_notes=None, analysis_duration_ms=1.0)
    r = AnalyzeResponse(**base)
    assert r.time_signature is None and r.meter_confidence is None and r.camelot is None
    r = AnalyzeResponse(**base, time_signature="3/4", meter_confidence=0.7, camelot="8B",
                        circle_of_fifths_index=0, relative_key="A minor",
                        parallel_key="C minor")
    assert r.model_dump()["time_signature"] == "3/4"


def test_advanced_key_relations_helper():
    from khensu_services.analysis.advanced_analyzer import _key_relations

    assert _key_relations("A minor") == {"camelot": "8A", "circle_of_fifths_index": 0,
                                         "relative_key": "C major",
                                         "parallel_key": "A major"}
    assert _key_relations("unknown")["camelot"] is None


# --- tempo octave folding + genre_targets.json convention -------------------------

def test_octave_folded_bpm_helpers():
    assert sa.octave_folded_bpm_distance(70.0, 140.0) == 0.0
    assert sa.octave_folded_bpm_distance(280.0, 140.0) == 0.0
    assert sa.octave_folded_bpm_distance(100.0, 140.0) == 40.0
    assert sa.fold_bpm_to_range(70.0, 130.0, 150.0) == 140.0
    assert sa.fold_bpm_to_range(300.0, 140.0, 160.0) == 150.0
    assert sa.fold_bpm_to_range(120.0, 100.0, 140.0) == 120.0   # in range: unchanged
    assert sa.bpm_in_range_folded(72.0, 138.0, 150.0)
    assert not sa.bpm_in_range_folded(100.0, 138.0, 150.0)
    assert sa.fold_bpm_to_window(50.0) == 100.0
    assert sa.fold_bpm_to_window(240.0) == 120.0
    assert sa.fold_bpm_to_window(0.0) == 0.0


def test_genre_tree_folds_out_of_window_tempo(monkeypatch):
    from khensu_services.analysis.genre_detector import estimate_genre_and_subgenre

    y = 0.1 * np.random.default_rng(1).standard_normal(SR * 2)
    assert estimate_genre_and_subgenre(y, SR, 50.0) == estimate_genre_and_subgenre(y, SR, 100.0)
    assert estimate_genre_and_subgenre(y, SR, 260.0) == estimate_genre_and_subgenre(y, SR, 130.0)


def test_genre_blend_tempo_folds_octaves_with_a_penalty(monkeypatch):
    """A half-time read still matches its full-time genre (octave folding),
    but a genre the tempo matches directly wins a tie (AUDIT-084: folding
    without a penalty put 70 and 140 BPM anchors at the same distance)."""
    import librosa

    from khensu_services.analysis import genre_detector as gd

    y = 0.1 * np.random.default_rng(2).standard_normal(SR * 3)
    centroid = float(librosa.feature.spectral_centroid(y=y, sr=SR).mean())
    feats = {"low_end_weight": 0.5, "crest_factor_db": 9.0}
    anchors = [("half-time", "x", 70.0, centroid, 0.5, 9.0),
               ("full-time", "x", 140.0, centroid, 0.5, 9.0),
               ("mid-tempo", "x", 100.0, centroid, 0.5, 9.0)]
    monkeypatch.setattr(gd, "_load_genre_anchors", lambda: anchors)
    at_70 = gd.estimate_genre_blend(y, SR, 70.0, min_rank_score=0.0, measured_features=feats)
    at_140 = gd.estimate_genre_blend(y, SR, 140.0, min_rank_score=0.0, measured_features=feats)
    assert at_70[0]["genre"] == "half-time" and at_70[1]["genre"] == "full-time"
    assert at_140[0]["genre"] == "full-time" and at_140[1]["genre"] == "half-time"
    # The folded match still beats a direct but 30 BPM-off anchor.
    assert at_70[1]["rank_score"] > at_70[2]["rank_score"]
    assert at_70[0]["rank_score"] > at_70[1]["rank_score"]


def test_octave_folded_distance_penalty():
    from khensu_services.analysis._shared_analysis import octave_folded_bpm_distance as d

    assert d(70.0, 140.0) == 0.0                       # no penalty by default
    assert d(70.0, 140.0, fold_penalty=8.0) == 8.0
    assert d(140.0, 140.0, fold_penalty=8.0) == 0.0
    assert d(72.0, 140.0, fold_penalty=8.0) == pytest.approx(12.0)
    assert d(100.0, 140.0, fold_penalty=8.0) == pytest.approx(40.0)  # min(40, 60+8, 60+8)


def _catalog() -> dict:
    path = Path(__file__).resolve().parents[2] / "resources" / "genre_norms" / "genre_targets.json"
    return json.loads(path.read_text(encoding="utf-8"))


def test_genre_targets_state_one_tempo_convention():
    data = _catalog()
    assert "DAW-project tempo" in data["tempo_convention"]
    by_id = {g["id"]: g for g in data["genres"]}
    for g in data["genres"]:
        lo, hi = g["typical_bpm_range"]
        assert 0 < lo <= hi, g["id"]
    # Half-time-feel lineages are quoted at their full-time grid, not ~70.
    full_time = ["trap", "latin-trap", "trap-soul", "trap-metal", "drill", "uk-drill",
                 "brooklyn-drill", "dubstep", "brostep", "riddim", "melodic-dubstep",
                 "chillstep", "cloud-rap", "emo-rap", "plugg", "pluggnb", "rage",
                 "drum-and-bass", "liquid-drum-and-bass", "neurofunk", "jungle"]
    for gid in full_time:
        assert by_id[gid]["typical_bpm_range"][0] >= 120, gid


# The re-gridded half-time-feel ranges (2026-10-15 sanity check): the felt
# tempo a beat tracker often reports must fold into the notated range, and the
# range must stay narrower than an octave or folding makes it match any tempo.
@pytest.mark.parametrize("gid,half_time_feel", [
    ("chillstep", (60, 70)),     # mostly quoted 120-140 notated
    ("cloud-rap", (60, 80)),     # slow, dreamy half-time trap
    ("latin-trap", (65, 85)),    # 130-170 counted double-time
    ("emo-rap", (60, 85)),
])
def test_regridded_half_time_ranges_fold_and_discriminate(gid, half_time_feel):
    by_id = {g["id"]: g for g in _catalog()["genres"]}
    lo, hi = by_id[gid]["typical_bpm_range"]
    assert hi < 2 * lo, gid
    for felt in range(half_time_feel[0], half_time_feel[1] + 1, 5):
        assert sa.bpm_in_range_folded(felt, lo, hi), (gid, felt)
        assert sa.bpm_in_range_folded(2 * felt, lo, hi), (gid, 2 * felt)


def test_genre_targets_still_validate_against_schema():
    from khensu_services.util.genre_schema import validate_genre_targets

    validate_genre_targets(_catalog())


# --- key relationships + Roman numerals --------------------------------------------

@pytest.mark.parametrize("tonic,mode,camelot,cof", [
    (0, "major", "8B", 0), (7, "major", "9B", 1), (5, "major", "7B", 11),
    (11, "major", "1B", 5), (10, "major", "6B", 10), (6, "major", "2B", 6),
    (9, "minor", "8A", 0), (4, "minor", "9A", 1), (2, "minor", "7A", 11),
    (8, "minor", "1A", 5), (0, "minor", "5A", 9), (3, "minor", "2A", 6),
])
def test_camelot_and_circle_of_fifths(tonic, mode, camelot, cof):
    assert hm.camelot_code(tonic, mode) == camelot
    assert hm.circle_of_fifths_index(tonic, mode) == cof


def test_camelot_codes_are_unique_over_24_keys():
    codes = {hm.camelot_code(pc, m) for pc in range(12) for m in ("major", "minor")}
    assert len(codes) == 24


def test_relative_and_parallel_keys_are_spelled_for_their_key():
    assert hm.relative_key(0, "major") == "A minor"
    assert hm.relative_key(9, "minor") == "C major"
    assert hm.relative_key(10, "major") == "G minor"
    assert hm.relative_key(2, "minor") == "F major"
    assert hm.parallel_key(0, "major") == "C minor"
    assert hm.parallel_key(1, "major") == "C# minor"   # not "Db minor" (8 flats)
    assert hm.parallel_key(3, "minor") == "Eb major"


@pytest.mark.parametrize("chord,roman,diatonic", [
    ("C", "I", True), ("Dm", "ii", True), ("Em7", "iii7", True), ("F", "IV", True),
    ("G7", "V7", True), ("Am", "vi", True), ("Bdim", "viio", True),
    ("Cmaj7", "Imaj7", True), ("Gsus4", "Vsus4", True), ("Dsus2", "IIsus2", True),
    ("Bb", "bVII", False), ("Ab", "bVI", False), ("Eb", "bIII", False),
    ("Fm", "iv", False), ("D7", "V7/V", False), ("F#dim", "#ivo", False),
    ("Db", "bII", False), ("N", None, None), ("", None, None), ("Xq", None, None),
])
def test_roman_numerals_in_c_major(chord, roman, diatonic):
    assert hm.roman_numeral(chord, 0, "major") == roman
    assert hm.is_diatonic(chord, 0, "major") is diatonic


@pytest.mark.parametrize("chord,roman,diatonic", [
    ("Am", "i", True), ("Bdim", "iio", True), ("C", "III", True), ("Dm", "iv", True),
    ("Em", "v", True), ("E7", "V7", True), ("F", "VI", True), ("G", "VII", True),
    ("G#dim", "viio", True), ("Bb", "bII", False), ("D", "IV", False),
    ("A", "I", False), ("Am7", "i7", True),
])
def test_roman_numerals_in_a_minor(chord, roman, diatonic):
    assert hm.roman_numeral(chord, 9, "minor") == roman
    assert hm.is_diatonic(chord, 9, "minor") is diatonic


def test_roman_numerals_with_flat_roots_in_flat_keys():
    assert hm.roman_numeral("Eb", 10, "major") == "IV"
    assert hm.roman_numeral("Bbm", 10, "major") == "i"
    assert hm.roman_numeral("Ab", 10, "major") == "bVII"
    assert hm.parse_chord("Bbm7") == (10, "m7")


def _pcs(*notes):
    v = np.zeros(12)
    v[list(notes)] = 1.0
    return v


def test_chord_sequence_reports_romans_and_key_relations():
    from khensu_services.analysis.performance_analyzer import detect_chord_sequence

    c, am, f, g7 = _pcs(0, 4, 7), _pcs(9, 0, 4), _pcs(5, 9, 0), _pcs(7, 11, 2, 5)
    chroma = np.concatenate([np.tile(s[:, None], (1, 21)) for s in [c, c, am, am, f, f, g7, g7] * 3],
                            axis=1)
    out = detect_chord_sequence(chroma, SR, 512)
    assert (out["key"], out["mode"]) == ("C", "major")
    assert (out["camelot"], out["circle_of_fifths_index"]) == ("8B", 0)
    assert (out["relative_key"], out["parallel_key"]) == ("A minor", "C minor")
    pairs = {(e["chord"], e["roman"]) for e in out["chords"]}
    assert {("C", "I"), ("Am", "vi"), ("F", "IV")} <= pairs
    assert any(r in ("V7", "V") for _c, r in pairs)
    assert all(e["diatonic"] for e in out["chords"])

    silent = detect_chord_sequence(np.zeros((12, 50)), SR, 512)
    assert silent["camelot"] is None and silent["relative_key"] is None
    assert all(e["roman"] is None and e["diatonic"] is None for e in silent["chords"])


# --- basic-pitch worker note table ----------------------------------------------------

def test_midi_bp_worker_uses_the_shared_note_table():
    from khensu_services.analysis import midi_bp_worker as w

    assert not hasattr(w, "_NOTE_NAMES")
    for m in range(128):
        assert w._name(m) == f"{nn.SHARP_NAMES[m % 12]}{m // 12 - 1}"
    assert w._name(60) == "C4" and w._name(70) == "A#4"
