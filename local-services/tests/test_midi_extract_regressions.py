"""MIDI-extraction findings from the 2026-10-15 audit (slice P2).

AUDIT-016 pyin fallback honours reference_hz (route + model field),
AUDIT-044 basic-pitch on its own executor, no cold-start stall for DSP callers,
          duration-scaled timeout, per-call failure info, kill()+wait(),
AUDIT-045 UTF-8 on both ends of the worker pipe.
"""
from __future__ import annotations

import sys
import textwrap
import time
import types

import numpy as np
import soundfile as sf

import khensu_services.analysis.midi_extract as me

SR = 22050

ECHO_WORKER = textwrap.dedent('''
    import json, sys
    print(json.dumps({"ready": True}), flush=True)
    for line in sys.stdin:
        print(json.dumps({"kind": "pitched", "notes": [], "n_notes": 0,
                          "extractor": "basic_pitch", "path": line.strip()}), flush=True)
''')


def _server(tmp_path):
    script = tmp_path / "echo_worker.py"
    script.write_text(ECHO_WORKER, encoding="utf-8")
    server = me._BasicPitchServer(worker_args=(str(script),))
    server.START_TIMEOUT = 30.0
    return server


# --- AUDIT-045 ---------------------------------------------------------------

def test_non_latin_path_round_trips_through_the_worker_pipe(tmp_path):
    server = _server(tmp_path)
    try:
        path = "C:/Musik/\u30c9\u30e9\u30e0_\u0431\u0430\u0441_\u00e9t\u00e9.wav"
        data, err = server.predict(sys.executable, path)
        assert err is None, err
        assert data["path"] == path
    finally:
        server.stop()


# --- AUDIT-044 ---------------------------------------------------------------

def test_stop_kills_and_reaps_the_worker(tmp_path):
    server = _server(tmp_path)
    assert server.warm_up(sys.executable) is None
    proc = server._proc
    assert server.is_ready()
    server.stop()
    assert proc.poll() is not None          # reaped, not a zombie
    assert not server.is_ready()


def test_cold_worker_does_not_stall_a_dsp_caller(tmp_path, monkeypatch):
    server = _server(tmp_path)
    monkeypatch.setattr(me, "_bp_server", server)
    monkeypatch.setattr(me, "_venv311_python", lambda: sys.executable)
    try:
        t0 = time.monotonic()
        data, err = me.run_basicpitch("a.wav", allow_cold_start=False)
        assert time.monotonic() - t0 < 1.0
        assert data is None and "starting" in err
        # ...while the cold start proceeds on the basic-pitch executor.
        deadline = time.monotonic() + 30.0
        while not server.is_ready() and time.monotonic() < deadline:
            time.sleep(0.05)
        assert server.is_ready()
        data, err = me.run_basicpitch("b.wav", allow_cold_start=False)
        assert err is None and data["path"] == "b.wav"
    finally:
        server.stop()


def test_basicpitch_runs_on_its_own_executor(tmp_path, monkeypatch):
    seen = []

    class _Recording:
        def predict(self, py, path, timeout=None):
            import threading
            seen.append(threading.current_thread().name)
            return {"kind": "pitched", "notes": []}, None

        def stop(self):
            pass

        def is_ready(self):
            return True

    monkeypatch.setattr(me, "_bp_server", _Recording())
    monkeypatch.setattr(me, "_venv311_python", lambda: sys.executable)
    me.run_basicpitch("x.wav")
    assert seen and seen[0].startswith("basic-pitch")


def test_request_timeout_scales_with_duration(monkeypatch):
    import soundfile
    monkeypatch.setattr(soundfile, "info", lambda p: types.SimpleNamespace(duration=600.0))
    assert me._request_timeout("long.wav") >= 900.0
    monkeypatch.setattr(soundfile, "info", lambda p: types.SimpleNamespace(duration=5.0))
    assert me._request_timeout("short.wav") == me._BasicPitchServer.REQUEST_TIMEOUT


def _tone(path, hz=220.0, secs=1.5):
    t = np.arange(int(SR * secs)) / SR
    sf.write(str(path), (0.4 * np.sin(2 * np.pi * hz * t)).astype(np.float32), SR)
    return str(path)


def test_failure_reason_is_reported_per_call(tmp_path):
    out = me.extract_midi(_tone(tmp_path / "a.wav"), "lead",
                          basic_pitch=(None, "worker gave no answer within 120s"), is_drums=False)
    assert out["extraction_backend"] == "librosa_pyin_monophonic"
    assert out["basic_pitch_fallback_reason"] == "worker gave no answer within 120s"
    clean = me.extract_midi(_tone(tmp_path / "b.wav"), "lead", basic_pitch=(None, None), is_drums=False)
    assert "basic_pitch_fallback_reason" not in clean


# --- AUDIT-016 ---------------------------------------------------------------

def _vibrato_note_432(secs=2.0):
    """C4 in A=432 tuning (-31.8 cents vs A=440) with a 3 Hz, +/-45 cent vibrato.

    Read against 440 each dip spends ~5 frames below the B3/C4 boundary,
    clear of the 5-frame median smoothing and 3-frame hysteresis. A 5 Hz
    +/-40 cent vibrato dipped only ~3 frames, so whether it split depended on
    pyin's 10-cent quantisation, and librosa 0.11's tracker kept it whole.
    """
    t = np.arange(int(SR * secs)) / SR
    base = 432.0 * 2 ** ((60 - 69) / 12.0)
    inst = base * 2 ** ((45.0 * np.sin(2 * np.pi * 3.0 * t)) / 1200.0)
    phase = 2 * np.pi * np.cumsum(inst) / SR
    return (0.4 * np.sin(phase)).astype(np.float32)


def test_pyin_segments_against_the_reference_pitch():
    y = _vibrato_note_432()
    at_432 = me.extract_pitched(y, SR, reference_hz=432.0)
    assert at_432["reference_hz"] == 432.0
    assert {n["midi"] for n in at_432["notes"]} == {60}
    at_440 = me.extract_pitched(y, SR)
    # Read against 440 the vibrato straddles the B3/C4 boundary and splits.
    assert at_440["n_notes"] > at_432["n_notes"]


def test_sound_recreation_and_midi_variation_forward_reference_hz(client, tmp_path, monkeypatch):
    p = tmp_path / "lead.wav"
    sf.write(str(p), _vibrato_note_432(), SR)
    seen = []

    def fake_extract(path, stem_type="auto", reference_hz=440.0, **kw):
        seen.append(reference_hz)
        return {"kind": "pitched", "notes": [{"start_sec": 0.0, "end_sec": 0.5, "midi": 60,
                                              "velocity": 90, "pitch_bend_cents": 0.0}],
                "n_notes": 1, "monophonic": True, "extraction_backend": "librosa_pyin_monophonic"}

    monkeypatch.setattr(me, "extract_midi", fake_extract)
    r = client.post("/analyze/sound_recreation", json={"file_path": str(p), "stem_type": "lead",
                                                       "reference_hz": 432.0})
    assert r.status_code == 200, r.text
    r = client.post("/analyze/midi_variation", json={"file_path": str(p), "stem_type": "lead",
                                                     "reference_hz": 432.0})
    assert r.status_code == 200, r.text
    assert seen == [432.0, 432.0]
    bad = client.post("/analyze/sound_recreation", json={"file_path": str(p), "reference_hz": 500.0})
    assert bad.status_code == 422
