"""Diatonic vocal-harmony MIDI export tests."""
from __future__ import annotations

import mido
import pytest

from khensu_services.analysis.vocal_harmony import diatonic_harmony_midi, generate_vocal_harmony, normalize_key


def test_key_normalization_and_diatonic_intervals():
    assert normalize_key("Bb") == ("Bb", 10)
    assert normalize_key("A#") == ("Bb", 10)            # flat-side key spelled with flats
    assert normalize_key("Gb", "minor") == ("F#", 6)    # F# minor is sharp-side
    assert diatonic_harmony_midi(60, 0, "major", "third", "above") == 64  # C -> E
    assert diatonic_harmony_midi(64, 0, "major", "third", "above") == 67  # E -> G
    assert diatonic_harmony_midi(72, 0, "major", "fifth", "below") == 65  # C -> F below
    with pytest.raises(ValueError, match="Unsupported key"):
        normalize_key("H")


@pytest.mark.parametrize("note, tonic, mode, interval, direction, expected", [
    # A minor (tonic pc 9): the old absolute-pitch-class arithmetic wrapped
    # through C and sent B4 a third "above" to D4 (-9 semitones).
    (71, 9, "minor", "third", "above", 74),   # B4 -> D5
    (72, 9, "minor", "third", "below", 69),   # C5 -> A4
    (69, 9, "minor", "third", "above", 72),   # A4 -> C5
    (76, 9, "minor", "fifth", "above", 83),   # E5 -> B5
    (67, 9, "minor", "third", "above", 71),   # G4 -> B4
    # G major (tonic pc 7)
    (66, 7, "major", "third", "above", 69),   # F#4 -> A4
    (67, 7, "major", "third", "below", 64),   # G4 -> E4
    (71, 7, "major", "third", "above", 74),   # B4 -> D5
    (60, 7, "major", "fifth", "above", 67),   # C4 -> G4
    # E major (tonic pc 4)
    (71, 4, "major", "third", "above", 75),   # B4 -> D#5
    (73, 4, "major", "third", "above", 76),   # C#5 -> E5
    (63, 4, "major", "third", "below", 59),   # D#4 -> B3
    (64, 4, "major", "fifth", "below", 57),   # E4 -> A3
])
def test_thirds_and_fifths_are_correct_in_every_key(note, tonic, mode, interval, direction, expected):
    out = diatonic_harmony_midi(note, tonic, mode, interval, direction)
    assert out == expected
    assert (out > note) == (direction == "above")


def test_generate_harmony_writes_channel_one_midi(tmp_path, monkeypatch):
    source = tmp_path / "vocal.wav"
    source.write_bytes(b"audio placeholder")
    output = tmp_path / "harmony.mid"
    monkeypatch.setattr("khensu_services.analysis.midi_extract.extract_midi", lambda *_args: {
        "kind": "pitched", "extraction_backend": "librosa_pyin_monophonic",
        "notes": [
            {"start_sec": 0.0, "end_sec": 0.5, "midi": 60, "velocity": 90},
            {"start_sec": 0.5, "end_sec": 1.0, "midi": 64, "velocity": 96},
        ],
    })
    result = generate_vocal_harmony(str(source), key="C", mode="major", interval="third",
                                    direction="above", bpm=120.0, output_path=str(output))
    assert result["n_events"] == 2
    assert [event["harmony_midi"] for event in result["events"]] == [64, 67]
    assert result["low_quality_extraction"] is True
    assert [event["harmony_note"] for event in result["events"]] == ["E4", "G4"]
    rendered = mido.MidiFile(str(output))
    note_ons = [message for message in rendered.tracks[0]
                if message.type == "note_on" and message.velocity > 0]
    assert [message.note for message in note_ons] == [64, 67]
    # MIDI channel 1 (mido channel 0), like every other export here.
    assert all(message.channel == 0 for message in note_ons)
    assert result["key_source"] == "request" and result["bpm_source"] == "request"
    meta = {m.type: m for m in rendered.tracks[0] if m.is_meta}
    assert meta["time_signature"].numerator == 4 and meta["time_signature"].denominator == 4
    assert mido.tempo2bpm(meta["set_tempo"].tempo) == pytest.approx(120.0, abs=1e-3)


@pytest.mark.parametrize("key, mode, expected", [
    ("F#", "major", ("F#", 6)),     # 6 sharps vs 6 flats: the caller's spelling stands
    ("Gb", "major", ("Gb", 6)),
    ("D#", "minor", ("D#", 3)),
    ("Eb", "minor", ("Eb", 3)),
    ("Gb", "minor", ("F#", 6)),     # Gb minor would need 9 flats
    ("A#", "major", ("Bb", 10)),
    ("C#", "major", ("Db", 1)),
])
def test_normalize_key_keeps_the_callers_spelling_only_for_enharmonic_ties(key, mode, expected):
    assert normalize_key(key, mode) == expected


def test_f_sharp_major_harmony_is_spelled_with_sharps(tmp_path, monkeypatch):
    source = tmp_path / "vocal.wav"
    source.write_bytes(b"audio placeholder")
    monkeypatch.setattr("khensu_services.analysis.midi_extract.extract_midi", lambda *_args: {
        "kind": "pitched", "extraction_backend": "basic_pitch",
        "notes": [{"start_sec": 0.0, "end_sec": 0.5, "midi": 66, "velocity": 90}],  # F#4
    })
    result = generate_vocal_harmony(str(source), key="F#", interval="third", direction="above",
                                    bpm=100.0, output_path=str(tmp_path / "h.mid"))
    assert result["key"] == "F#" and result["mode"] == "major"
    assert result["events"][0]["source_note"] == "F#4"
    assert result["events"][0]["harmony_note"] == "A#4"     # was "Bb4" under a Gb respelling


def test_key_strings_carry_their_mode():
    from khensu_services.analysis.vocal_harmony import resolve_harmony_context

    ctx = resolve_harmony_context("unused.wav", "F#m", None, 90.0)
    assert (ctx["key"], ctx["mode"], ctx["key_source"]) == ("F#", "minor", "request")
    ctx = resolve_harmony_context("unused.wav", "Bb minor", None, 90.0)
    assert (ctx["key"], ctx["mode"]) == ("Bb", "minor")
    ctx = resolve_harmony_context("unused.wav", "A", "minor", 90.0)   # explicit mode wins
    assert (ctx["key"], ctx["mode"]) == ("A", "minor")
    with pytest.raises(ValueError, match="mode was given without a key"):
        resolve_harmony_context("unused.wav", None, "minor", 90.0)


def _chord_loop(path, bpm=120.0, sr=22050, bars=12):
    """A minor i-iv-v-i pads with a kick on every beat."""
    import numpy as np
    import soundfile as sf

    beat = 60.0 / bpm
    n = int(bars * 4 * beat * sr) + sr
    y = np.zeros(n)
    t_beat = np.arange(int(beat * sr)) / sr
    chords = [[57, 60, 64], [62, 65, 69], [64, 67, 71], [57, 60, 64]]   # Am Dm Em Am
    rng = np.random.default_rng(1)
    for b in range(bars * 4):
        start = int(round(b * beat * sr))
        seg = slice(start, start + len(t_beat))
        for m in chords[(b // 4) % 4]:
            y[seg] += 0.08 * np.sin(2 * np.pi * 440.0 * 2 ** ((m - 69) / 12) * t_beat)
        k = int(0.05 * sr)
        tk = np.arange(k) / sr
        y[start:start + k] += 0.6 * np.sin(2 * np.pi * 60 * tk) * np.exp(-tk * 40)
        y[start:start + k] += 0.1 * rng.standard_normal(k) * np.exp(-tk * 120)
    sf.write(str(path), (0.8 * y / np.max(np.abs(y))).astype(np.float32), sr)


def test_key_and_tempo_are_detected_when_not_given(tmp_path):
    from khensu_services.analysis.vocal_harmony import resolve_harmony_context

    src = tmp_path / "loop.wav"
    _chord_loop(src, bpm=120.0)
    ctx = resolve_harmony_context(str(src), None, None, None)
    assert ctx["key_source"] == "detected" and ctx["bpm_source"] == "detected"
    assert (ctx["key"], ctx["mode"]) in {("A", "minor"), ("C", "major")}
    assert ctx["bpm"] == pytest.approx(120.0, abs=0.1)
    assert 0.0 <= ctx["key_confidence"] <= 1.0


def test_endpoint_422s_instead_of_assuming_c_major_120(client, tmp_path, monkeypatch):
    import numpy as np
    import soundfile as sf

    silent = tmp_path / "silent.wav"
    sf.write(str(silent), np.zeros(22050 * 2, dtype=np.float32), 22050)
    called = []
    monkeypatch.setattr("khensu_services.analysis.vocal_harmony.generate_vocal_harmony",
                        lambda *a, **k: called.append(a) or {})
    r = client.post("/analyze/vocal_harmony", json={"file_path": str(silent)})
    assert r.status_code == 422, r.text
    assert "key" in r.json()["detail"]
    r = client.post("/analyze/vocal_harmony", json={"file_path": str(silent), "key": "C"})
    assert r.status_code == 422, r.text
    assert "bpm" in r.json()["detail"]
    assert not called


def test_vocal_harmony_endpoint_runs_off_event_loop(client, tmp_path, monkeypatch):
    import numpy as np
    import soundfile as sf

    source = tmp_path / "vocal.wav"
    # Real (if silent) audio: the decode-budget guard 422s a placeholder.
    sf.write(str(source), np.zeros(2205, dtype=np.float32), 22050)
    monkeypatch.setattr("khensu_services.analysis.vocal_harmony.generate_vocal_harmony", lambda *_args: {
        "output_path": "harmony.mid", "n_events": 1, "events": [], "key": "C",
        "mode": "major", "interval": "third", "direction": "above", "bpm": 120.0,
        "extraction_backend": "test", "low_quality_extraction": False,
    })
    response = client.post("/analyze/vocal_harmony", json={"file_path": str(source), "key": "C",
                                                            "bpm": 120.0})
    assert response.status_code == 200, response.text
    assert response.json()["output_path"] == "harmony.mid"
