"""Music-theory audit fixes (2026-10-15, slice P4).

* AUDIT-015 - reported BPM is the beat-fit tempo, not the quantised
  tempogram bin, everywhere (analyzers, the exported .mid header);
* AUDIT-051 - octave-less chroma notes take the shortest signed pitch-class
  interval in melodic contours;
* AUDIT-052 - exported MIDI carries a time_signature meta; reports print the
  BPM / key confidence and a low-quality-extraction caveat;
* AUDIT-084 - sections follow the detected meter and downbeat phase; minor-key
  Roman numerals use viio / secondary-dominant conventions.
"""
from __future__ import annotations

import mido
import numpy as np
import pytest
import soundfile as sf

from khensu_services.analysis import _harmony as hm
from khensu_services.analysis._shared_analysis import refine_bpm, tracked_tempo


def _clicks(bpm: float, sr: int, seconds: float = 20.0, seed: int = 0) -> np.ndarray:
    rng = np.random.default_rng(seed)
    beat = 60.0 / bpm
    y = np.zeros(int(seconds * sr), dtype=np.float32)
    n = int(0.03 * sr)
    tt = np.arange(n) / sr
    click = (np.sin(2 * np.pi * 80 * tt) * np.exp(-tt * 80)).astype(np.float32)
    for b in range(int((seconds - 0.5) / beat)):
        start = int(round(b * beat * sr))
        y[start:start + n] += click + (0.3 * rng.standard_normal(n) * np.exp(-tt * 200)).astype(np.float32)
    return y


# --- AUDIT-015: beat-fit tempo -------------------------------------------------

@pytest.mark.parametrize("sr", [22050, 44100, 48000])
@pytest.mark.parametrize("bpm", [90.0, 120.0, 128.0, 140.0, 174.0])
def test_tracked_tempo_is_within_a_tenth_of_a_bpm(bpm, sr):
    import librosa

    y = _clicks(bpm, sr)
    tempo, frames = librosa.beat.beat_track(y=y, sr=sr)
    beats = librosa.frames_to_time(frames, sr=sr)
    assert tracked_tempo(tempo, beats) == pytest.approx(bpm, abs=0.1)


def test_refine_bpm_survives_a_missed_and_an_extra_beat():
    period = 60.0 / 128.0
    beats = [i * period for i in range(40)]
    del beats[17]                                   # missed beat (a 2-period gap)
    beats.insert(25, beats[24] + 0.1 * period)       # spurious extra beat
    beats[30] += 0.4 * period                        # one badly placed beat
    assert refine_bpm(beats, 129.2) == pytest.approx(128.0, abs=0.05)


def test_refine_bpm_falls_back_below_eight_beats():
    assert refine_bpm([0.0, 0.5, 1.0, 1.5], 117.45) == 117.45
    assert refine_bpm([], 99.0) == 99.0
    assert tracked_tempo(np.array([143.55]), [i * 60.0 / 140.0 for i in range(32)]) == \
        pytest.approx(140.0, abs=1e-6)


def test_quick_analyzer_reports_the_beat_fit_tempo(tmp_path, monkeypatch):
    from khensu_services.analysis import onnx_inference
    from khensu_services.analysis.quick_analyzer import analyze_quick

    monkeypatch.setattr(onnx_inference, "predict_key_tempo_onnx", lambda *_a, **_k: None)
    path = tmp_path / "clicks.wav"
    sf.write(str(path), _clicks(140.0, 22050, seconds=24.0), 22050)
    assert analyze_quick(str(path))["bpm"] == pytest.approx(140.0, abs=0.1)


def test_midi_variation_header_tempo_equals_the_reported_bpm(tmp_path, monkeypatch):
    from khensu_services.analysis.midi_variation import apply_midi_variation

    path = tmp_path / "clicks.wav"
    sf.write(str(path), _clicks(140.0, 44100, seconds=24.0), 44100)
    monkeypatch.setattr("khensu_services.analysis.midi_extract.extract_midi", lambda *_a, **_k: {
        "kind": "pitched", "extraction_backend": "basic_pitch",
        "notes": [{"start_sec": 0.0, "end_sec": 0.4, "midi": 60, "velocity": 90}],
    })
    out = tmp_path / "v.mid"
    result = apply_midi_variation(str(path), output_path=str(out))
    assert result["bpm"] == pytest.approx(140.0, abs=0.1)        # was 143.55
    assert result["bpm_source"] == "detected"
    meta = {m.type: m for m in mido.MidiFile(str(out)).tracks[0] if m.is_meta}
    assert mido.tempo2bpm(meta["set_tempo"].tempo) == pytest.approx(result["bpm"], abs=0.01)
    sig = meta["time_signature"]
    assert f"{sig.numerator}/{sig.denominator}" == result["time_signature"]


# --- AUDIT-052: MIDI time signature ------------------------------------------------

@pytest.mark.parametrize("signature, expected, clocks", [
    (None, (4, 4), 24), ("3/4", (3, 4), 24), ("6/8", (6, 8), 36), ("7/9", (4, 4), 24),
])
def test_midi_writer_emits_a_time_signature_meta(signature, expected, clocks):
    from khensu_services.common.midi_io import MidiNote, build_midi_file

    midi = build_midi_file([MidiNote(0.0, 0.5, 60, 90)], 120.0, time_signature=signature)
    sigs = [m for m in midi.tracks[0] if m.type == "time_signature"]
    assert len(sigs) == 1
    assert (sigs[0].numerator, sigs[0].denominator) == expected
    assert sigs[0].clocks_per_click == clocks and sigs[0].time == 0


# --- AUDIT-051: chroma contour --------------------------------------------------

def test_chroma_contour_uses_the_shortest_pitch_class_interval():
    from khensu_services.analysis.hook_analyzer import _melodic_contour, pitch_class_interval

    assert pitch_class_interval(71, 60) == 1       # B -> C: up a semitone
    assert pitch_class_interval(60, 71) == -1      # C -> B: down a semitone
    assert pitch_class_interval(60, 67) == -5      # C -> G: nearest is down a fourth
    chroma_notes = [{"start_sec": i * 0.5, "midi_pitch": 60 + pc, "velocity": 0.5,
                     "pitch_class_only": True} for i, pc in enumerate((9, 11, 0, 2))]  # A B C D
    assert _melodic_contour(0.0, 10.0, chroma_notes) == [1, 1, 1]  # was [1, -1, 1]
    # Real transcriptions keep their actual interval (an octave-plus leap up).
    real = [{"start_sec": 0.0, "midi_pitch": 60, "velocity": 0.5},
            {"start_sec": 0.5, "midi_pitch": 79, "velocity": 0.5}]
    assert _melodic_contour(0.0, 10.0, real) == [1]


def test_chroma_fallback_flags_its_notes_as_octaveless():
    from khensu_services.analysis.advanced_analyzer import _chroma_fallback_pitch_analysis

    sr = 22050
    t = np.arange(sr * 2) / sr
    y = np.concatenate([np.sin(2 * np.pi * 493.88 * t), np.sin(2 * np.pi * 523.25 * t)])  # B4, C5
    _key, _conf, notes = _chroma_fallback_pitch_analysis(y.astype(np.float32), sr)
    assert notes and all(n["pitch_class_only"] for n in notes)


# --- AUDIT-084: sections on the detected meter -----------------------------------

def test_sections_follow_detected_meter_and_downbeat_phase(monkeypatch):
    import librosa

    from khensu_services.analysis import _shared_analysis as sa
    from khensu_services.analysis.section_analyzer import detect_sections

    sr = 22050
    period = 0.5
    beat_times = np.arange(0.25, 120.0, period)        # 240 beats
    frames = librosa.time_to_frames(beat_times, sr=sr)
    monkeypatch.setattr(librosa.beat, "beat_track", lambda **_k: (np.array([117.45]), frames))
    seen = {}

    def fake_meter(y, sr_, beats, bpm=None):
        seen["bpm"] = bpm
        return {"time_signature": "3/4", "beats_per_bar": 3, "meter_confidence": 0.8,
                "downbeat_phase": 2}
    monkeypatch.setattr(sa, "estimate_meter", fake_meter)
    y = np.zeros(int(sr * 121.0), dtype=np.float32)
    sections = detect_sections(y, sr)
    starts = [s["start_sec"] for s in sections]
    real = librosa.frames_to_time(frames, sr=sr)
    # 8 bars of 3/4 = 24 beats per section, starting at the first downbeat
    # (beat index 2); the pickup before it is merged into section 1.
    expected = [0.0] + [float(real[i]) for i in range(2 + 24, len(real), 24)]
    assert starts == pytest.approx(expected)
    assert seen["bpm"] == pytest.approx(120.0, abs=0.1)   # beat-fit, not 117.45


# --- AUDIT-084: Roman numerals ------------------------------------------------------

@pytest.mark.parametrize("chord, tonic, mode, roman", [
    ("G#dim", 9, "minor", "viio"),       # leading-tone chord in A minor (was "#viio")
    ("G#dim7", 9, "minor", "viio7"),
    ("G#m7b5", 9, "minor", "viim7b5"),
    ("G", 9, "minor", "VII"),            # the subtonic stays VII
    ("E7", 9, "minor", "V7"),
    ("B7", 9, "minor", "V7/V"),          # was "II7"
    ("G7", 9, "minor", "V7/III"),
    ("A7", 9, "minor", "V7/iv"),
    ("D7", 0, "major", "V7/V"),          # was "II7"
    ("E7", 0, "major", "V7/vi"),
    ("A7", 0, "major", "V7/ii"),
    ("C7", 0, "major", "V7/IV"),
    ("G7", 0, "major", "V7"),
    ("Bb7", 0, "major", "bVII7"),        # resolves to bIII, not a diatonic target
    ("F#7", 0, "major", "#IV7"),         # would tonicize vii (diminished): no V7/
    ("D", 0, "major", "II"),             # a triad is not labelled as a secondary dominant
])
def test_roman_numeral_conventions(chord, tonic, mode, roman):
    assert hm.roman_numeral(chord, tonic, mode) == roman


# --- AUDIT-052: report confidence + caveat ------------------------------------------

def _report_request(tmp_path, **extra):
    from khensu_services.report.models import ReportRequest

    return ReportRequest(session_name="s", file_name="f.wav", computed_at="2026-10-15T00:00:00",
                         lufs_integrated=-10.0, lra=6.0, true_peak_dbtp=-1.0,
                         crest_factor_db=9.0, dynamic_range_db=8.0, stereo_width=0.5,
                         bpm=128.0, pitch_key="A minor",
                         output_path=str(tmp_path / "r.html"), **extra)


def test_report_caveat_only_when_estimates_are_weak(tmp_path):
    from khensu_services.report.generator_pdf import estimate_caveat

    assert estimate_caveat(_report_request(tmp_path)) == ""
    assert estimate_caveat(_report_request(tmp_path, bpm_confidence=0.9, key_confidence=0.8)) == ""
    weak = estimate_caveat(_report_request(tmp_path, bpm_confidence=0.9, key_confidence=0.3))
    assert "key estimate is low-confidence" in weak and "BPM" not in weak
    mono = estimate_caveat(_report_request(tmp_path, low_quality_extraction=True))
    assert "monophonic" in mono


def test_html_report_shows_confidence_and_caveat(tmp_path):
    from khensu_services.report.generator import generate_html_report

    path = generate_html_report(_report_request(tmp_path, bpm_confidence=0.92, key_confidence=0.31,
                                                low_quality_extraction=True))
    html = open(path, encoding="utf-8").read()
    assert "confidence 92 %" in html and "confidence 31 %" in html
    assert "low-confidence" in html and "monophonic" in html


def test_pdf_report_shows_confidence(tmp_path):
    pytest.importorskip("reportlab")
    from khensu_services.report import generator_pdf

    req = _report_request(tmp_path, bpm_confidence=0.92, key_confidence=0.31)
    assert generator_pdf._with_confidence("128.0", req.bpm_confidence) == "128.0 (confidence 92%)"
    assert generator_pdf._with_confidence("A minor", None) == "A minor"
