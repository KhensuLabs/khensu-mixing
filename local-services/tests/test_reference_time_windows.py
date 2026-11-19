"""Tests for the dynamic (time-varying) reference match — _compute_windowed_metrics
and its wiring into the /analyze/reference endpoint's time_windows field."""
from __future__ import annotations

import numpy as np
import pytest
import soundfile as sf
from fastapi.testclient import TestClient

from khensu_services.analysis.router_audio import _compute_windowed_metrics
from khensu_services.server import app

client = TestClient(app)


def _write_tone(path, sr, duration_sec, freq_hz, amp=0.3):
    t = np.arange(int(sr * duration_sec)) / sr
    y = (amp * np.sin(2 * np.pi * freq_hz * t)).astype(np.float32)
    sf.write(str(path), np.stack([y, y], axis=-1), sr)


def test_windowed_metrics_returns_empty_for_short_clip(tmp_path):
    p = tmp_path / "short.wav"
    _write_tone(p, 44100, duration_sec=2.0, freq_hz=440.0)  # < 8s needed for 8 windows
    windows = _compute_windowed_metrics(str(p), n_windows=8)
    assert windows == []


def test_windowed_metrics_produces_n_windows_for_long_clip(tmp_path):
    p = tmp_path / "long.wav"
    _write_tone(p, 44100, duration_sec=16.0, freq_hz=440.0)
    windows = _compute_windowed_metrics(str(p), n_windows=8)
    assert len(windows) == 8
    for w in windows:
        assert w["end_sec"] > w["start_sec"]
        assert len(w["octave_rms_db"]) == 10
        assert np.isfinite(w["centroid_hz"])
        assert np.isfinite(w["crest_db"])


def test_windowed_metrics_windows_are_contiguous_and_cover_full_duration(tmp_path):
    p = tmp_path / "long.wav"
    _write_tone(p, 44100, duration_sec=16.0, freq_hz=440.0)
    windows = _compute_windowed_metrics(str(p), n_windows=4)
    assert windows[0]["start_sec"] == 0.0
    assert windows[-1]["end_sec"] == pytest.approx(16.0, abs=0.01)
    for a, b in zip(windows, windows[1:]):
        assert a["end_sec"] == pytest.approx(b["start_sec"], abs=0.01)


def test_windowed_metrics_detects_brightness_change_over_time(tmp_path):
    # First half dark (200 Hz), second half bright (6000 Hz) — the windowed
    # centroid must rise from the first half of windows to the second.
    sr = 44100
    half_n = sr * 4
    t = np.arange(half_n) / sr
    dark = 0.3 * np.sin(2 * np.pi * 200.0 * t)
    bright = 0.3 * np.sin(2 * np.pi * 6000.0 * t)
    y = np.concatenate([dark, bright]).astype(np.float32)
    p = tmp_path / "shifting.wav"
    sf.write(str(p), np.stack([y, y], axis=-1), sr)

    windows = _compute_windowed_metrics(str(p), n_windows=8)
    assert len(windows) == 8
    first_half_centroid = np.mean([w["centroid_hz"] for w in windows[:4]])
    second_half_centroid = np.mean([w["centroid_hz"] for w in windows[4:]])
    assert second_half_centroid > first_half_centroid * 5


def test_reference_endpoint_includes_time_windows_for_long_tracks(tmp_path):
    target = tmp_path / "target.wav"
    reference = tmp_path / "reference.wav"
    _write_tone(target, 44100, duration_sec=16.0, freq_hz=440.0)
    _write_tone(reference, 44100, duration_sec=16.0, freq_hz=880.0)

    resp = client.post("/analyze/reference", json={
        "target_path": str(target), "reference_path": str(reference),
    })
    assert resp.status_code == 200, resp.text
    data = resp.json()
    assert len(data["time_windows"]) == 8
    w0 = data["time_windows"][0]
    assert w0["window_index"] == 0
    assert len(w0["delta_octave_db"]) == 10
    # 880 Hz reference should read consistently brighter than 440 Hz target.
    assert all(w["delta_centroid_hz"] < 0 for w in data["time_windows"])


def test_reference_endpoint_time_windows_empty_for_short_tracks(tmp_path):
    target = tmp_path / "target.wav"
    reference = tmp_path / "reference.wav"
    _write_tone(target, 44100, duration_sec=1.0, freq_hz=440.0)
    _write_tone(reference, 44100, duration_sec=1.0, freq_hz=440.0)

    resp = client.post("/analyze/reference", json={
        "target_path": str(target), "reference_path": str(reference),
    })
    assert resp.status_code == 200, resp.text
    assert resp.json()["time_windows"] == []


def test_compare_match_eq_is_the_reference_chain_eq(tmp_path):
    # CON-9: one engine - the Compare card's match curve and the chain that
    # renders it are the same bands.
    from khensu_services.copilot.reference_chain import build_reference_chain
    rng = np.random.default_rng(3)
    sr = 44100
    noise = rng.standard_normal(sr * 4).astype(np.float32) * 0.1
    dark = np.convolve(noise, np.ones(32) / 32, mode="same").astype(np.float32)  # low-passed
    tgt, ref = tmp_path / "bright.wav", tmp_path / "dark.wav"
    sf.write(str(tgt), np.stack([noise, noise], axis=-1), sr)
    sf.write(str(ref), np.stack([dark, dark], axis=-1), sr)
    r = client.post("/analyze/reference", json={"target_path": str(tgt), "reference_path": str(ref)})
    assert r.status_code == 200, r.text
    body = r.json()
    chain = build_reference_chain(str(tgt), str(ref))
    peaks = [e["parameters"] for e in chain["effects"] if e["effect"] == "PeakFilter"]
    assert peaks, "a bright target against a dark reference needs a match-EQ"
    assert body["match_eq_center_hz"] == [p["cutoff_frequency_hz"] for p in peaks]
    assert body["match_eq_gain_db"] == [p["gain_db"] for p in peaks]
