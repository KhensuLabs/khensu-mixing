# local-services/tests/test_sound_recreation.py
"""Tests for MIDI extraction + sound recreation."""
import numpy as np
import soundfile as sf

from khensu_services.analysis.midi_extract import extract_pitched
from khensu_services.analysis.sound_recreation import analyze_sound_recreation, PLUGIN_KB

SR = 44100


def _write(tmp_path, name, mono):
    p = tmp_path / name
    sf.write(str(p), np.asarray(mono, dtype=np.float32), SR)
    return str(p)


def _note(midi, dur):
    hz = 440.0 * 2 ** ((midi - 69) / 12.0)
    t = np.arange(int(dur * SR)) / SR
    return 0.4 * np.sin(2 * np.pi * hz * t)


def test_midi_extract_recovers_a_melody(tmp_path):
    # C3 -> E3 -> G3 monophonic line.
    seq = np.concatenate([_note(48, 0.5), _note(52, 0.5), _note(55, 0.5)])
    r = extract_pitched(seq.astype(np.float32), SR)
    assert r["kind"] == "pitched"
    assert r["n_notes"] >= 3
    got = sorted({n["midi"] for n in r["notes"]})
    # The three target pitches should appear (allow +/-1 semitone tolerance).
    for target in (48, 52, 55):
        assert any(abs(m - target) <= 1 for m in got)


def test_sub_bass_classified_and_recommended(tmp_path):
    # Pure low sine -> sub category.
    path = _write(tmp_path, "sub.wav", _note(31, 3.0))   # ~49 Hz
    r = analyze_sound_recreation(path, stem_type="bass")
    assert r["category"] in ("sub", "808", "bass")
    assert len(r["recommendations"]) >= 1
    assert "plugin" in r["recommendations"][0] and "chain" in r["recommendations"][0]


def test_bright_lead_classified_lead(tmp_path):
    t = np.arange(int(2.0 * SR)) / SR
    # Bright sawtooth-ish high tone.
    y = sum(0.2 / k * np.sin(2 * np.pi * 880 * k * t) for k in range(1, 8))
    path = _write(tmp_path, "lead.wav", y)
    r = analyze_sound_recreation(path, stem_type="lead")
    assert r["category"] in ("lead", "guitar", "pad")


def test_kb_covers_all_categories():
    for cat in ("sub", "808", "bass", "reese", "lead", "pad", "fx", "guitar",
                "kick", "snare", "hat", "perc"):
        assert cat in PLUGIN_KB and PLUGIN_KB[cat]


def test_midi_summary_carries_the_extraction_backend(tmp_path):
    # RecreateSoundPanel flags a monophonic pyin fallback from these two keys
    # (AUDIT-052); the summary whitelist used to strip them, so the caveat
    # never showed (2026-10-16 C++/Python contract check).
    path = _write(tmp_path, "lead.wav", _note(60, 1.0))
    mono = {"kind": "pitched", "n_notes": 1, "pitch_min_midi": 60, "pitch_max_midi": 60,
            "has_glide": False, "extraction_backend": "librosa_pyin_monophonic"}
    ms = analyze_sound_recreation(path, stem_type="lead", midi=mono)["midi_summary"]
    assert ms["extraction_backend"] == "librosa_pyin_monophonic"
    assert ms["low_quality_extraction"] is True

    poly = dict(mono, extraction_backend="basic_pitch_polyphonic")
    ms = analyze_sound_recreation(path, stem_type="lead", midi=poly)["midi_summary"]
    assert ms["low_quality_extraction"] is False
