"""Unified per-window mix metric timeline (pipeline #6).

Covers the new windowed harshness curve (localizes the previously whole-track
scalar) and the /analyze/metric_timeline endpoint that aligns loudness, crest,
and harshness per section with a hotspot summary.
"""
from __future__ import annotations

import numpy as np
import soundfile as sf
from fastapi.testclient import TestClient

from khensu_services.analysis.extended_analyzer import (
    analyze_harshness, windowed_harshness_curve)
from khensu_services.server import app

client = TestClient(app)
SR = 44100


def _stereo(mono):
    return np.stack([mono, mono]).astype(np.float32)


def _harsh_tone(secs=8.0, hz=3500.0):
    # 3.5 kHz sits in the 2.5-5 kHz harsh band -> high harshness.
    t = np.arange(int(SR * secs)) / SR
    return (0.4 * np.sin(2 * np.pi * hz * t)).astype(np.float32)


def _clean_tone(secs=8.0, hz=200.0):
    t = np.arange(int(SR * secs)) / SR
    return (0.4 * np.sin(2 * np.pi * hz * t)).astype(np.float32)


# --- windowed harshness curve ----------------------------------------------

def test_windowed_harshness_high_for_harsh_tone_low_for_clean():
    harsh = windowed_harshness_curve(_stereo(_harsh_tone()), SR)
    clean = windowed_harshness_curve(_stereo(_clean_tone()), SR)
    assert len(harsh) > 1 and len(clean) > 1
    assert np.mean(harsh) > 0.5           # 3.5 kHz reads as harsh
    assert np.mean(clean) < 0.2           # 200 Hz does not


def test_windowed_harshness_mean_tracks_the_scalar_score():
    y = _stereo(_harsh_tone())
    curve = windowed_harshness_curve(y, SR)
    scalar = analyze_harshness(y, SR)["score"]
    # The windowed mean and the whole-track scalar measure the same ratio, so
    # they land close (both saturate near 1 for a pure harsh tone).
    assert abs(float(np.mean(curve)) - float(scalar)) < 0.1


def test_windowed_harshness_is_deterministic():
    y = _stereo(_harsh_tone())
    assert windowed_harshness_curve(y, SR) == windowed_harshness_curve(y, SR)


def test_windowed_harshness_localizes_a_harsh_section():
    # First half clean, second half harsh -> later windows score higher.
    y = np.concatenate([_clean_tone(4.0), _harsh_tone(4.0)])
    curve = windowed_harshness_curve(_stereo(y), SR)
    half = len(curve) // 2
    assert np.mean(curve[:half]) < np.mean(curve[half:])


# --- endpoint ---------------------------------------------------------------

def test_metric_timeline_endpoint_aligns_curves_and_flags_hotspots(tmp_path):
    y = np.concatenate([_clean_tone(4.0), _harsh_tone(4.0)])
    src = tmp_path / "mix.wav"
    sf.write(str(src), _stereo(y).T, SR)

    resp = client.post("/analyze/metric_timeline", json={"file_path": str(src)})
    assert resp.status_code == 200, resp.text
    body = resp.json()
    assert body["n_windows"] == len(body["windows"]) > 1
    row = body["windows"][0]
    assert {"start_sec", "end_sec", "loudness_lufs", "crest_db", "harshness"} <= set(row)
    # Clean intro, harsh outro -> the timeline localizes it, and the hotspot is
    # past the clean intro windows (not window 0).
    assert body["windows"][0]["harshness"] < 0.2
    assert body["windows"][-1]["harshness"] > 0.5
    harshest = body["hotspots"]["harshest"]
    assert harshest is not None and harshest["window_index"] >= 2


def test_metric_timeline_endpoint_404_for_missing_file():
    resp = client.post("/analyze/metric_timeline", json={"file_path": "/no/such.wav"})
    assert resp.status_code == 404
