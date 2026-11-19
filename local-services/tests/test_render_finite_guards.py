"""Finite/NaN validation across the render paths beyond dsp.py (audit CQ-01).

The core apply_pedalboard_chain/apply_graph/render_delta/apply_section_chains
paths already validate finiteness (via _run_serial_chain and
_atomic_render_write). This sweeps the *other* audio-writing render paths -
platform master, crossfeed A/B, gain rider, dither preview, comping - which
previously only np.clip'd their output (clipping a NaN leaves it NaN), so a
corrupt source could reach the written file undetected.
"""
from __future__ import annotations

import numpy as np
import pytest
import soundfile as sf

from khensu_services.copilot.dsp import require_finite_audio


def _write_nan_wav(path, sr=44100, n=44100, channels=1):
    y = np.zeros((n, channels), dtype=np.float32) if channels > 1 else np.zeros(n, dtype=np.float32)
    y.reshape(-1)[n // 2] = np.nan
    sf.write(str(path), y, sr, subtype="FLOAT")


def test_require_finite_audio_passes_clean_audio():
    y = np.array([0.1, -0.2, 0.3], dtype=np.float64)
    # Returns the array unchanged so it can wrap a read inline.
    assert require_finite_audio(y) is y


@pytest.mark.parametrize("bad", [np.nan, np.inf, -np.inf])
def test_require_finite_audio_rejects_non_finite(bad):
    y = np.array([0.1, bad, 0.3], dtype=np.float64)
    with pytest.raises(ValueError, match="non-finite"):
        require_finite_audio(y)


def test_platform_master_rejects_non_finite_source(tmp_path):
    from khensu_services.copilot.platform_render import render_platform_master
    src = tmp_path / "nan.wav"
    _write_nan_wav(src)
    with pytest.raises(ValueError, match="non-finite"):
        render_platform_master(str(src), str(tmp_path / "out.wav"), target_lufs=-14.0, ceiling_dbtp=-1.0)
    assert not (tmp_path / "out.wav").exists()


def test_crossfeed_rejects_non_finite_source(tmp_path):
    from khensu_services.copilot.crossfeed import render_crossfeed_ab
    src = tmp_path / "nan.wav"
    _write_nan_wav(src, channels=2)
    with pytest.raises(ValueError, match="non-finite"):
        render_crossfeed_ab(str(src), str(tmp_path / "cf"))


def test_gain_rider_rejects_non_finite_source(tmp_path):
    from khensu_services.analysis.gain_rider import compute_gain_ride
    src = tmp_path / "nan.wav"
    _write_nan_wav(src)
    with pytest.raises(ValueError, match="non-finite"):
        compute_gain_ride(str(src), render=True)


def test_dither_preview_rejects_non_finite_source(tmp_path):
    from khensu_services.analysis.dither_preview import analyze_dither
    src = tmp_path / "nan.wav"
    _write_nan_wav(src)
    with pytest.raises(ValueError, match="non-finite"):
        analyze_dither(str(src), target_bits=16, out_dir=str(tmp_path))


def test_comping_rejects_non_finite_take(tmp_path):
    # comping loads takes mono=True, so librosa's own valid_audio rejects a
    # NaN take (ParameterError) before our defense-in-depth check even runs -
    # either way it fails closed and writes nothing.
    from khensu_services.analysis.comping import analyze_comping
    good = tmp_path / "good.wav"
    bad = tmp_path / "bad.wav"
    sf.write(str(good), np.zeros(44100, dtype=np.float32), 44100)
    _write_nan_wav(bad)
    with pytest.raises(Exception):
        analyze_comping([str(good), str(bad)], render=True)
    assert not list(tmp_path.glob("*_comp*.wav"))
