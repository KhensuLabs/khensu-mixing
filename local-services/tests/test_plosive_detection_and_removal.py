"""Regression tests for vocal plosive candidate analysis and low-band removal."""
from __future__ import annotations

import numpy as np
import pytest
from scipy.signal import butter, sosfiltfilt

from khensu_services.analysis.vocal_analyzer import analyze_vocal
from khensu_services.copilot.dsp import _apply_plosive_remover, _run_serial_chain


SR = 44_100


def _low_rms(audio: np.ndarray, lo: int, hi: int) -> float:
    sos = butter(4, [lo / (SR * 0.5), hi / (SR * 0.5)], btype="band", output="sos")
    return float(np.sqrt(np.mean(sosfiltfilt(sos, audio) ** 2)))


def _plosive_fixture() -> np.ndarray:
    t = np.arange(SR * 2, dtype=np.float64) / SR
    vocal = 0.18 * np.sin(2.0 * np.pi * 220.0 * t)
    burst = np.zeros_like(t)
    start, end = int(0.8 * SR), int(0.87 * SR)
    burst[start:end] = 0.85 * np.sin(2.0 * np.pi * 90.0 * t[:end - start])
    return vocal + burst


def test_vocal_analysis_reports_low_frequency_onset_candidate():
    result = analyze_vocal(_plosive_fixture(), SR)
    assert result["plosive_markers"]
    marker = max(result["plosive_markers"], key=lambda item: item["severity"])
    assert marker["time_sec"] == pytest.approx(0.8, abs=0.06)
    assert 0.0 <= marker["severity"] <= 1.0
    assert 0.55 <= marker["low_band_ratio"] <= 1.0


def test_plosive_remover_ducks_burst_but_preserves_sustained_voice_body():
    audio = _plosive_fixture()
    out = _apply_plosive_remover(audio, SR, frequency_hz=180.0,
                                 threshold_db=-40.0, reduction_db=18.0,
                                 attack_ms=0.5, release_ms=50.0)
    burst = slice(int(0.80 * SR), int(0.87 * SR))
    body = slice(int(1.20 * SR), int(1.70 * SR))
    assert _low_rms(out[burst], 50, 180) < 0.65 * _low_rms(audio[burst], 50, 180)
    assert _low_rms(out[body], 50, 180) > 0.9 * _low_rms(audio[body], 50, 180)


def test_plosive_remover_chain_dispatch_and_parameter_clamps():
    mono = _plosive_fixture()
    rendered, applied, skipped, _reports, _levels = _run_serial_chain(np.stack([mono, mono]), SR, [
        {"effect": "PlosiveRemover", "parameters": {
            "frequency_hz": 99_999.0, "threshold_db": -99.0,
            "reduction_db": 99.0, "attack_ms": 0.0, "release_ms": 9_999.0,
        }},
    ])
    assert applied == ["PlosiveRemover"]
    assert skipped == []
    assert rendered.shape == (2, mono.size)
    assert np.isfinite(rendered).all()
