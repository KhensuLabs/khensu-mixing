"""Tests for dsp.analyze_saturation_aliasing (near-Nyquist harmonic report)
and its wiring into apply_pedalboard_chain/apply_graph for TapeSaturation stages."""
from __future__ import annotations

import numpy as np
import pytest
import soundfile as sf

from khensu_services.copilot.dsp import (
    analyze_saturation_aliasing, apply_pedalboard_chain, apply_graph,
)


def test_identical_signal_reports_zero_added_energy():
    sr = 44100
    t = np.arange(sr) / sr
    y = 0.3 * np.sin(2 * np.pi * 200.0 * t)
    report = analyze_saturation_aliasing(y, y, sr)
    assert report["near_nyquist_added_db"] == pytest.approx(0.0, abs=1e-6)
    assert report["broadband_added_db"] == pytest.approx(0.0, abs=1e-6)


def test_hard_clipping_adds_near_nyquist_energy():
    # A driven sine clipped hard generates strong high-order harmonics —
    # exactly the un-oversampled aliasing scenario this report is meant to
    # surface.
    sr = 44100
    t = np.arange(sr) / sr
    dry = 0.3 * np.sin(2 * np.pi * 200.0 * t)
    wet = np.clip(dry * 20.0, -1.0, 1.0)
    report = analyze_saturation_aliasing(dry, wet, sr)
    assert report["near_nyquist_added_db"] > 3.0
    assert report["broadband_added_db"] > 0.0


def test_handles_stereo_input():
    sr = 44100
    t = np.arange(sr) / sr
    dry = np.stack([0.3 * np.sin(2 * np.pi * 200.0 * t)] * 2)
    wet = np.clip(dry * 10.0, -1.0, 1.0)
    report = analyze_saturation_aliasing(dry, wet, sr)
    assert np.isfinite(report["near_nyquist_added_db"])
    assert np.isfinite(report["broadband_added_db"])


def test_apply_pedalboard_chain_includes_harmonic_report_for_tape_saturation(tmp_path):
    sr = 44100
    t = np.arange(sr) / sr
    y = (0.4 * np.sin(2 * np.pi * 220.0 * t)).astype(np.float32)
    src = tmp_path / "in.wav"
    out = tmp_path / "out.wav"
    sf.write(str(src), y, sr)

    result = apply_pedalboard_chain(str(src), str(out), [
        {"effect": "TapeSaturation", "parameters": {"drive_db": 18.0, "warmth": 0.5}},
    ])
    assert "harmonic_reports" in result
    assert len(result["harmonic_reports"]) == 1
    assert result["harmonic_reports"][0]["effect"] == "TapeSaturation"
    assert "near_nyquist_added_db" in result["harmonic_reports"][0]


def test_apply_pedalboard_chain_no_report_without_saturation(tmp_path):
    sr = 44100
    t = np.arange(sr) / sr
    y = (0.4 * np.sin(2 * np.pi * 220.0 * t)).astype(np.float32)
    src = tmp_path / "in.wav"
    out = tmp_path / "out.wav"
    sf.write(str(src), y, sr)

    result = apply_pedalboard_chain(str(src), str(out), [
        {"effect": "Gain", "parameters": {"gain_db": -3.0}},
    ])
    assert result["harmonic_reports"] == []


def test_apply_graph_includes_harmonic_report_with_branch_label(tmp_path):
    sr = 44100
    t = np.arange(sr) / sr
    y = (0.4 * np.sin(2 * np.pi * 220.0 * t)).astype(np.float32)
    src = tmp_path / "in.wav"
    out = tmp_path / "out.wav"
    sf.write(str(src), y, sr)

    result = apply_graph(str(src), str(out), {
        "post": [{"effect": "TapeSaturation", "parameters": {"drive_db": 18.0}}],
    })
    assert len(result["harmonic_reports"]) == 1
    assert result["harmonic_reports"][0]["branch"] == "post"
