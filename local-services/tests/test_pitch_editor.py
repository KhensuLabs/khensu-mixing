"""Tests for the pitch note-grid editor backend (/analyze/pitch_curve,
/render/pitch_correct)."""
import numpy as np
import soundfile as sf

from khensu_services.analysis.pitch_editor import analyze_pitch_curve, render_pitch_correct

SR = 22050


def _tone(freq, sec, amp=0.5, sr=SR):
    t = np.arange(int(sec * sr)) / sr
    return (amp * np.sin(2 * np.pi * freq * t)).astype(np.float32)


def _write(tmp_path, name, y, sr=SR):
    p = tmp_path / name
    sf.write(str(p), y, sr, subtype="FLOAT")
    return str(p)


def _peak_freq(x, sr):
    win = np.hanning(len(x))
    spec = np.abs(np.fft.rfft(x * win, n=8 * len(x)))
    k = int(np.argmax(spec))
    a, b, c = np.log(spec[k - 1:k + 2] + 1e-12)
    k = k + 0.5 * (a - c) / (a - 2 * b + c)
    return k * sr / (8 * len(x))


def test_pitch_curve_segments_two_notes(tmp_path):
    # A3 (57) for 0.6 s, silence, then C4 (60) for 0.6 s.
    y = np.concatenate([_tone(220.0, 0.6), np.zeros(int(0.3 * SR), np.float32),
                        _tone(261.63, 0.6)])
    res = analyze_pitch_curve(_write(tmp_path, "v.wav", y))
    assert abs(res["duration_sec"] - 1.5) < 0.01
    assert res["hop_sec"] > 0
    assert len(res["f0_midi"]) <= 4000
    assert any(v is None for v in res["f0_midi"])          # the silent gap
    notes = res["notes"]
    assert [n["target_midi"] for n in notes] == [57, 60]
    assert [n["id"] for n in notes] == [0, 1]
    assert abs(notes[0]["midi"] - 57.0) < 0.2
    assert notes[0]["end_sec"] <= notes[1]["start_sec"]
    assert abs(notes[1]["start_sec"] - 0.9) < 0.08


def test_pitch_curve_splits_on_jump_without_gap(tmp_path):
    y = np.concatenate([_tone(220.0, 0.5), _tone(246.94, 0.5)])  # A3 -> B3, legato
    notes = analyze_pitch_curve(_write(tmp_path, "v.wav", y))["notes"]
    assert [n["target_midi"] for n in notes] == [57, 59]


def test_pitch_curve_downsamples_long_files(tmp_path, monkeypatch):
    import khensu_services.analysis.pitch_editor as pe
    monkeypatch.setattr(pe, "MAX_CURVE_POINTS", 50)
    res = pe.analyze_pitch_curve(_write(tmp_path, "v.wav", _tone(220.0, 1.0)))
    assert len(res["f0_midi"]) <= 50
    assert abs(res["hop_sec"] * len(res["f0_midi"]) - 1.0) < 0.1


def test_pitch_correct_shifts_region_only(tmp_path):
    y = _tone(220.0, 3.0)
    src = _write(tmp_path, "v.wav", y)
    res = render_pitch_correct(src, [{"start_sec": 1.0, "end_sec": 2.0, "shift_semitones": 2.0}])
    assert res["applied"] == 1
    out, sr = sf.read(res["rendered_path"])
    assert sr == SR and len(out) == len(y)
    mid = out[int(1.25 * SR):int(1.75 * SR)]
    assert abs(_peak_freq(mid, sr) - 246.94) < 2.0
    before = out[int(0.2 * SR):int(0.7 * SR)]
    assert abs(_peak_freq(before, sr) - 220.0) < 1.0
    np.testing.assert_allclose(out[: int(0.8 * SR)], y[: int(0.8 * SR)], atol=1e-6)


def test_pitch_correct_ignores_tiny_and_invalid(tmp_path):
    y = _tone(220.0, 1.0)
    src = _write(tmp_path, "v.wav", y)
    res = render_pitch_correct(src, [
        {"start_sec": 0.1, "end_sec": 0.5, "shift_semitones": 0.001},   # too small
        {"start_sec": 0.6, "end_sec": 0.4, "shift_semitones": 3.0},     # inverted
        {"start_sec": 5.0, "end_sec": 6.0, "shift_semitones": 3.0},     # out of range
    ])
    assert res["applied"] == 0
    out, _ = sf.read(res["rendered_path"])
    np.testing.assert_allclose(out, y, atol=1e-6)


def test_pitch_correct_preserves_channels_and_clamps(tmp_path):
    st = np.stack([_tone(220.0, 1.5), _tone(220.0, 1.5, amp=0.25)], axis=1)
    src = _write(tmp_path, "st.wav", st)
    res = render_pitch_correct(src, [{"start_sec": 0.5, "end_sec": 1.0, "shift_semitones": 40.0}])
    out, sr = sf.read(res["rendered_path"])
    assert out.shape == st.shape and sr == SR
    # Clamped to +12: one octave up.
    assert abs(_peak_freq(out[int(0.6 * SR):int(0.9 * SR), 0], sr) - 440.0) < 4.0


def test_endpoints(client, tmp_path):
    src = _write(tmp_path, "v.wav", _tone(220.0, 1.0))
    r = client.post("/analyze/pitch_curve", json={"file_path": src})
    assert r.status_code == 200
    body = r.json()
    assert set(body) == {"duration_sec", "hop_sec", "f0_midi", "notes", "reference_hz",
                         "measurement_reference_hz", "detected_reference_hz",
                         "reference_offset_cents", "reference_offset_warning"}
    assert set(body["notes"][0]) == {"id", "start_sec", "end_sec", "midi", "target_midi"}

    out = str(tmp_path / "custom.wav")
    r = client.post("/render/pitch_correct", json={
        "file_path": src, "output_path": out,
        "corrections": [{"start_sec": 0.2, "end_sec": 0.6, "shift_semitones": -1.0}]})
    assert r.status_code == 200
    assert r.json() == {"rendered_path": out, "applied": 1}

    r = client.post("/render/pitch_correct", json={"file_path": src, "corrections": []})
    assert r.status_code == 200
    assert r.json()["rendered_path"].endswith("_pitchcorrected.wav")

    r = client.post("/render/pitch_correct", json={"file_path": src, "output_path": src,
                                                   "corrections": []})
    assert r.status_code == 409
    r = client.post("/analyze/pitch_curve", json={"file_path": str(tmp_path / "nope.wav")})
    assert r.status_code == 404


def test_render_rejects_output_outside_allowed_roots(client, tmp_path):
    from pathlib import Path

    src = _write(tmp_path, "v.wav", _tone(220.0, 0.5))
    forbidden = str(Path(tmp_path.anchor) / "khensu_forbidden_pitch.wav")
    r = client.post("/render/pitch_correct", json={"file_path": src, "output_path": forbidden,
                                                   "corrections": []})
    assert r.status_code == 403
    assert not Path(forbidden).exists()
