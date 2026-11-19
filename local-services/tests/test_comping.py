# local-services/tests/test_comping.py
"""Tests for the Comping Assistant."""
import numpy as np
import soundfile as sf

from khensu_services.analysis.comping import analyze_comping

SR = 22050


def _take(tmp_path, name, wobble_cents=0.0, amp=0.3, phrase_amps=None):
    """Two 2s 'phrases' at 220 Hz with 1s gaps; optional pitch wobble."""
    if phrase_amps is None:
        phrase_amps = [amp, amp]
    parts = [np.zeros(int(0.5 * SR), dtype=np.float32)]
    for pa in phrase_amps:
        t = np.arange(int(2.0 * SR)) / SR
        wobble = 2.0 ** ((wobble_cents / 1200.0) * np.sin(2 * np.pi * 3.0 * t))
        phase = np.cumsum(220.0 * wobble) / SR
        parts.append((pa * np.sin(2 * np.pi * phase)).astype(np.float32))
        parts.append(np.zeros(int(1.0 * SR), dtype=np.float32))
    y = np.concatenate(parts)
    p = tmp_path / name
    sf.write(str(p), y, SR)
    return str(p)


def test_phrases_detected(tmp_path):
    a = _take(tmp_path, "a.wav")
    b = _take(tmp_path, "b.wav")
    r = analyze_comping([a, b])
    assert len(r["phrases"]) == 2
    assert r["take_count"] == 2


def test_stable_take_beats_wobbly_take(tmp_path):
    stable = _take(tmp_path, "stable.wav", wobble_cents=3.0)
    wobbly = _take(tmp_path, "wobbly.wav", wobble_cents=90.0)
    r = analyze_comping([stable, wobbly])
    for p in r["phrases"]:
        assert p["best_take"] == 0


def test_render_writes_comp(tmp_path):
    a = _take(tmp_path, "a.wav")
    b = _take(tmp_path, "b.wav", wobble_cents=80.0)
    r = analyze_comping([a, b], render=True)
    assert r["rendered_path"] is not None
    y, sr = sf.read(r["rendered_path"])
    assert len(y) > 0 and sr == SR


def test_endpoint_happy_path(client, tmp_path):
    a = _take(tmp_path, "a.wav")
    b = _take(tmp_path, "b.wav", wobble_cents=60.0)
    resp = client.post("/analyze/comping", json={"take_paths": [a, b]})
    assert resp.status_code == 200
    body = resp.json()
    assert body["take_count"] == 2
    assert all("best_take" in p for p in body["phrases"])


def test_endpoint_needs_two_takes(client, tmp_path):
    a = _take(tmp_path, "a.wav")
    resp = client.post("/analyze/comping", json={"take_paths": [a]})
    assert resp.status_code == 422
