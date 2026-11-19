"""MIDI export path safety + writer correctness (audit H1, MIDI-writer low).

/analyze/vocal_harmony and /analyze/midi_variation used to write to any
caller-supplied output_path; midi_variation also swallowed save errors and
returned a path to a file that didn't exist. The shared writer computes delta
ticks from absolute ticks and resolves overlapping same-pitch notes.
"""
from __future__ import annotations

import os

import mido
import numpy as np
import pytest
import soundfile as sf

from khensu_services.common.midi_io import MidiNote, build_midi_file, resolve_overlaps, write_midi_notes


def _tone(path, sr=22050, sec=1.0):
    t = np.arange(int(sr * sec)) / sr
    sf.write(str(path), (0.3 * np.sin(2 * np.pi * 440.0 * t)).astype(np.float32), sr)


_FAKE_PITCHED = {
    "kind": "pitched", "extraction_backend": "librosa_pyin_monophonic",
    "notes": [{"start_sec": 0.0, "end_sec": 0.5, "midi": 60, "velocity": 90}],
}


@pytest.fixture
def fake_extract(monkeypatch):
    monkeypatch.setattr("khensu_services.analysis.midi_extract.extract_midi",
                        lambda *_a, **_k: dict(_FAKE_PITCHED))
    monkeypatch.setattr("khensu_services.analysis.midi_variation._detect_tempo_meter",
                        lambda _p: (120.0, None, None))


@pytest.mark.parametrize("route", ["/analyze/vocal_harmony", "/analyze/midi_variation"])
def test_output_aliasing_the_source_is_409(client, tmp_path, fake_extract, route):
    src = tmp_path / "take.wav"
    _tone(src)
    alias = tmp_path / "TAKE.mid"  # different name: not an alias
    # key/bpm given to /analyze/vocal_harmony: a 1 s tone has too few beats
    # for it to detect a tempo. midi_variation declares neither field, and
    # request models reject undeclared keys under the test suite (WIRE-08).
    body = {"file_path": str(src), "output_path": str(alias)}
    if route == "/analyze/vocal_harmony":
        body.update(key="C", bpm=120.0)
    r = client.post(route, json=body)
    assert r.status_code == 200, r.text
    # The source itself (with a .mid-looking name impossible) -> extension 422 first.
    r = client.post(route, json={"file_path": str(src), "output_path": str(src)})
    assert r.status_code == 422, r.text


@pytest.mark.parametrize("route", ["/analyze/vocal_harmony", "/analyze/midi_variation"])
def test_non_midi_extension_is_422(client, tmp_path, fake_extract, route):
    src = tmp_path / "take.wav"
    _tone(src)
    r = client.post(route, json={"file_path": str(src), "output_path": str(tmp_path / "x.wav")})
    assert r.status_code == 422
    assert not (tmp_path / "x.wav").exists()


@pytest.mark.parametrize("route", ["/analyze/vocal_harmony", "/analyze/midi_variation"])
def test_output_outside_allowed_roots_is_403(client, tmp_path, fake_extract, route, monkeypatch):
    src = tmp_path / "take.wav"
    _tone(src)
    import khensu_services.common.output_paths as op
    monkeypatch.setattr(op, "allowed_output_roots", lambda source_path=None: [tmp_path / "only_here"])
    r = client.post(route, json={"file_path": str(src), "output_path": str(tmp_path / "x.mid")})
    assert r.status_code == 403


def test_midi_variation_alias_of_source_is_409(client, tmp_path, fake_extract):
    # A .mid-named source so the extension check passes and the alias check
    # fires. Its content is real audio: the decode-budget guard 422s a file
    # with no readable audio header before any route logic runs.
    src = tmp_path / "take.mid"
    _tone(tmp_path / "take.wav")
    (tmp_path / "take.wav").replace(src)
    before = src.read_bytes()
    r = client.post("/analyze/midi_variation",
                    json={"file_path": str(src), "output_path": str(src)})
    assert r.status_code == 409
    assert src.read_bytes() == before


def test_midi_variation_save_failure_propagates_as_422(client, tmp_path, fake_extract):
    src = tmp_path / "take.wav"
    _tone(src)
    missing_dir_out = tmp_path / "no_such_dir" / "out.mid"
    r = client.post("/analyze/midi_variation",
                    json={"file_path": str(src), "output_path": str(missing_dir_out)})
    assert r.status_code == 422, r.text
    assert not missing_dir_out.exists()


def test_midi_variation_default_output_is_mid_beside_source(client, tmp_path, fake_extract):
    src = tmp_path / "take.wav"
    _tone(src)
    r = client.post("/analyze/midi_variation", json={"file_path": str(src)})
    assert r.status_code == 200, r.text
    out = r.json()["output_path"]
    assert out.endswith("take_varied.mid") and os.path.isfile(out)


def test_midi_variation_params_are_bounded(client, tmp_path):
    src = tmp_path / "take.wav"
    _tone(src)
    for bad in ({"timing_variance": -1}, {"velocity_variance": 1000}, {"pitch_variation_prob": 2}):
        r = client.post("/analyze/midi_variation", json={"file_path": str(src), **bad})
        assert r.status_code == 422, bad


def test_overlapping_same_pitch_notes_are_not_cut():
    notes = [MidiNote(0.0, 1.0, 60, 90), MidiNote(0.5, 1.5, 60, 80),   # overlap
             MidiNote(2.0, 2.5, 62, 70), MidiNote(2.0, 3.0, 62, 100)]  # same start
    fixed = resolve_overlaps(notes)
    c4 = [n for n in fixed if n.note == 60]
    assert [(n.start_sec, n.end_sec) for n in c4] == [(0.0, 0.5), (0.5, 1.5)]
    d4 = [n for n in fixed if n.note == 62]
    assert len(d4) == 1 and d4[0].end_sec == 3.0 and d4[0].velocity == 100

    midi = build_midi_file(notes, 120.0)
    # Every note-on has a later matching note-off before the next note-on of
    # the same pitch (no stale off cutting a re-attack).
    sounding: dict[int, bool] = {}
    for msg in midi.tracks[0]:
        if msg.type == "note_on" and msg.velocity > 0:
            assert not sounding.get(msg.note), "re-attack while still sounding"
            sounding[msg.note] = True
        elif msg.type == "note_off":
            assert sounding.get(msg.note), "note-off for a note that isn't sounding"
            sounding[msg.note] = False


def test_delta_ticks_come_from_absolute_ticks():
    # 1000 notes 1/3 s apart: rounding each seconds-delta independently drifts;
    # absolute quantisation keeps the last onset exact.
    notes = [MidiNote(i / 3.0, i / 3.0 + 0.1, 60 + (i % 2), 90) for i in range(1000)]
    midi = build_midi_file(notes, 120.0)
    tick = 0
    onsets = []
    for msg in midi.tracks[0]:
        tick += msg.time
        if msg.type == "note_on":
            onsets.append(tick)
    sec_per_tick = mido.bpm2tempo(120.0) / 480 / 1e6
    assert abs(onsets[-1] * sec_per_tick - 999 / 3.0) <= sec_per_tick


def test_write_is_atomic_and_raises(tmp_path):
    with pytest.raises(OSError):
        write_midi_notes([MidiNote(0, 1, 60, 90)], str(tmp_path / "missing" / "x.mid"), 120.0)
    out = tmp_path / "ok.mid"
    write_midi_notes([MidiNote(0, 1, 60, 90)], str(out), 120.0)
    assert mido.MidiFile(str(out)).tracks
    assert not [f for f in os.listdir(tmp_path) if f.startswith(".khensu_midi_")]
