"""MIDI extraction bridge to the basic-pitch 3.11 sub-venv.

The structural tests run everywhere (no sub-venv needed); the real basic-pitch
path is exercised only when `.venv311` has been set up locally.
"""
import numpy as np
import soundfile as sf

import khensu_services.analysis.midi_extract as me

SR = 22050


def _melody(tmp_path):
    def note(m, d):
        hz = 440.0 * 2 ** ((m - 69) / 12.0)
        t = np.arange(int(d * SR)) / SR
        return 0.4 * np.sin(2 * np.pi * hz * t)
    y = np.concatenate([note(60, 0.6), note(64, 0.6), note(67, 0.6)]).astype(np.float32)
    p = tmp_path / "mel.wav"
    sf.write(str(p), y, SR)
    return str(p)


def test_bridge_returns_none_without_venv(tmp_path, monkeypatch):
    monkeypatch.setattr(me, "_venv311_python", lambda: None)
    assert me.extract_pitched_basicpitch(_melody(tmp_path)) is None


def test_extract_midi_falls_back_to_librosa(tmp_path, monkeypatch):
    # With basic-pitch unavailable, extract_midi uses the librosa.pyin path.
    monkeypatch.setattr(me, "extract_pitched_basicpitch", lambda path, **k: None)
    r = me.extract_midi(_melody(tmp_path), "lead", allow_cold_start=True)
    assert r["kind"] == "pitched"
    assert r["n_notes"] >= 3
    got = sorted({n["midi"] for n in r["notes"]})
    for target in (60, 64, 67):
        assert any(abs(m - target) <= 1 for m in got)


def test_basicpitch_when_venv_present(tmp_path):
    # Integration: only runs where the 3.11 sub-venv exists.
    if me._venv311_python() is None:
        import pytest
        pytest.skip("no .venv311 basic-pitch sub-venv")
    r = me.extract_midi(_melody(tmp_path), "lead", allow_cold_start=True)
    assert r.get("extractor") == "basic_pitch", f"bridge fell back: {me.last_basicpitch_failure}"
    got = sorted({n["midi"] for n in r["notes"]})
    for target in (60, 64, 67):
        assert target in got
    # The worker stays warm: a second extraction skips the multi-minute
    # import/model start-up that used to blow the per-call timeout.
    import time

    t0 = time.monotonic()
    again = me.extract_midi(_melody(tmp_path), "lead", allow_cold_start=True)
    assert again.get("extractor") == "basic_pitch", f"bridge fell back: {me.last_basicpitch_failure}"
    assert time.monotonic() - t0 < 60.0
