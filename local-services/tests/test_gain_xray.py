# local-services/tests/test_gain_xray.py
"""Tests for the Compression X-Ray (original vs processed gain diff)."""
import numpy as np
import soundfile as sf

from khensu_services.analysis.gain_xray import analyze_gain_xray

SR = 22050


def _write(tmp_path, name, y):
    p = tmp_path / name
    sf.write(str(p), y.astype(np.float32), SR)
    return str(p)


def _program(dur=12.0):
    """Loud/quiet alternating tone — something for a compressor to grab."""
    t = np.arange(int(dur * SR)) / SR
    amp = np.where((t % 4.0) < 2.0, 0.5, 0.1)
    return (amp * np.sin(2 * np.pi * 300.0 * t)).astype(np.float32)


def _simple_compress(y, threshold=0.15, ratio=4.0):
    """Instant-attack RMS compressor over 20 ms blocks."""
    win = int(0.020 * SR)
    out = y.copy()
    for i in range(0, len(y) - win, win):
        seg = out[i : i + win]
        rms = float(np.sqrt(np.mean(seg ** 2)))
        if rms > threshold:
            gain = (threshold + (rms - threshold) / ratio) / rms
            out[i : i + win] = seg * gain
    return out


def test_transparent_when_identical(tmp_path):
    y = _program()
    a = _write(tmp_path, "a.wav", y)
    b = _write(tmp_path, "b.wav", y)
    r = analyze_gain_xray(a, b)
    assert r["max_reduction_db"] < 0.5
    assert r["verdict"].startswith("transparent")
    # Identical signals => no tonal movement either.
    assert r["brightness_curve_db"], "brightness curve should be populated"
    assert len(r["brightness_curve_db"]) == len(r["gain_curve_db"])
    assert max(abs(v) for v in r["brightness_curve_db"]) < 0.5


def test_brightness_curve_tracks_tonal_shift(tmp_path):
    """A high-pass-ish tilt (boost highs) should read as positive brightness delta."""
    y = _program()
    t = np.arange(len(y)) / SR
    highs = 0.25 * np.sin(2 * np.pi * 6000.0 * t).astype(np.float32)
    a = _write(tmp_path, "a.wav", y)
    b = _write(tmp_path, "b.wav", (y + highs).astype(np.float32))
    r = analyze_gain_xray(a, b)
    assert len(r["brightness_curve_db"]) == len(r["gain_curve_db"])
    mean_bright = sum(r["brightness_curve_db"]) / max(1, len(r["brightness_curve_db"]))
    assert mean_bright > 1.0, "added 6 kHz energy should raise the brightness delta"


def test_static_gain_not_reported_as_compression(tmp_path):
    y = _program()
    a = _write(tmp_path, "a.wav", y)
    b = _write(tmp_path, "b.wav", y * 0.5)   # -6 dB static trim
    r = analyze_gain_xray(a, b)
    assert abs(r["static_offset_db"] + 6.0) < 1.0
    assert r["max_reduction_db"] < 1.0       # no *dynamic* movement


def test_compression_detected(tmp_path):
    y = _program()
    a = _write(tmp_path, "a.wav", y)
    b = _write(tmp_path, "b.wav", _simple_compress(y))
    r = analyze_gain_xray(a, b)
    assert r["max_reduction_db"] > 2.0
    assert len(r["gain_curve_db"]) > 0


def test_endpoint_happy_path(client, tmp_path):
    y = _program()
    a = _write(tmp_path, "a.wav", y)
    b = _write(tmp_path, "b.wav", _simple_compress(y))
    resp = client.post("/analyze/gain_xray",
                       json={"original_path": a, "processed_path": b})
    assert resp.status_code == 200
    body = resp.json()
    assert body["max_reduction_db"] > 0.0
    assert isinstance(body["events"], list)


def test_endpoint_missing_file_404(client, tmp_path):
    a = _write(tmp_path, "a.wav", _program(2.0))
    resp = client.post("/analyze/gain_xray",
                       json={"original_path": a, "processed_path": "Z:/no.wav"})
    assert resp.status_code == 404
