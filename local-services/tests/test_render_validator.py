# local-services/tests/test_render_validator.py
"""Tests for the blocking pre-render safety validator."""
import numpy as np
import pytest
import soundfile as sf

from khensu_services.copilot.dsp import _clamped_params
from khensu_services.copilot.render_validator import validate_render_request

SR = 44100


def _write(tmp_path, name, y, sr=SR):
    p = tmp_path / name
    sf.write(str(p), np.asarray(y, dtype=np.float32), sr)
    return str(p)


def _stereo(dur=2.0, sr=SR):
    t = np.arange(int(dur * sr)) / sr
    m = 0.3 * np.sin(2 * np.pi * 200.0 * t)
    return np.stack([m, m * 0.95], axis=1)


def test_valid_stereo_gain_passes(tmp_path):
    path = _write(tmp_path, "ok.wav", _stereo())
    v = validate_render_request(path, effects=[{"effect": "Gain", "parameters": {"gain_db": 2.0}}])
    assert v["ok"] is True
    assert v["channels"] == 2


def test_too_short_is_blocked(tmp_path):
    path = _write(tmp_path, "short.wav", _stereo(dur=0.2))
    v = validate_render_request(path, effects=[])
    assert v["ok"] is False
    assert any("too short" in b.lower() for b in v["blocks"])


def test_extreme_widening_on_decorrelated_low_end_blocks(tmp_path):
    # Anti-phase low end -> low mono correlation; extreme widening should block.
    t = np.arange(int(2.0 * SR)) / SR
    bass = 0.5 * np.sin(2 * np.pi * 60.0 * t)
    stereo = np.stack([bass, -bass], axis=1)   # hard anti-phase
    path = _write(tmp_path, "antiphase.wav", stereo)
    v = validate_render_request(path, effects=[{"effect": "StereoWidener", "parameters": {"width": 1.9}}])
    assert v["ok"] is False
    assert any("mono cancellation" in b for b in v["blocks"])


def test_high_makeup_gain_warns(tmp_path):
    path = _write(tmp_path, "ok.wav", _stereo())
    v = validate_render_request(path, effects=[{"effect": "Gain", "parameters": {"gain_db": 12.0}}])
    assert v["ok"] is True   # a warning, not a block
    assert any("make-up" in w or "gain" in w for w in v["warnings"])


def test_dsp_frequency_clamp_respects_source_nyquist():
    params = _clamped_params(
        "LowpassFilter", {"cutoff_frequency_hz": 20_000.0}, sample_rate=8_000)
    assert params["cutoff_frequency_hz"] == 3_600.0


def test_dsp_rejects_non_finite_parameter():
    with pytest.raises(ValueError, match="Non-finite DSP parameter"):
        _clamped_params("Gain", {"gain_db": float("nan")}, sample_rate=44_100)
