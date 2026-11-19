"""Tests for apply_section_chains — audit CQ-01: the pass-through (no
effects) branch skips _run_serial_chain entirely, which is where the
finite check normally lives, so a corrupt source section must be
validated independently."""
from __future__ import annotations

import numpy as np
import pytest
import soundfile as sf

from khensu_services.copilot.dsp import apply_section_chains


def _write_wav(path, y, sr=44100):
    sf.write(str(path), y.astype(np.float32), sr, subtype="FLOAT")


def test_apply_section_chains_rejects_non_finite_pass_through_section(tmp_path):
    sr = 44100
    n = sr  # 1 second
    y = np.zeros(n, dtype=np.float32)
    y[sr // 2] = np.nan
    in_path = tmp_path / "in.wav"
    out_path = tmp_path / "out.wav"
    _write_wav(in_path, y, sr)

    sections = [
        {"label": "verse", "start_sec": 0.0, "end_sec": 1.0, "effects": []},
    ]

    with pytest.raises(ValueError, match="non-finite"):
        apply_section_chains(str(in_path), str(out_path), sections)


def test_apply_section_chains_passes_through_finite_section_unmodified(tmp_path):
    sr = 44100
    n = sr
    t = np.arange(n) / sr
    y = (0.3 * np.sin(2 * np.pi * 440.0 * t)).astype(np.float32)
    in_path = tmp_path / "in.wav"
    out_path = tmp_path / "out.wav"
    _write_wav(in_path, y, sr)

    sections = [
        {"label": "verse", "start_sec": 0.0, "end_sec": 1.0, "effects": []},
    ]

    result = apply_section_chains(str(in_path), str(out_path), sections)
    assert result["output_path"] == str(out_path)

    out, _ = sf.read(str(out_path))
    assert np.isfinite(out).all()


def test_apply_section_chains_preserves_unlisted_audio(tmp_path):
    sr = 44100
    n = sr
    t = np.arange(n) / sr
    y = (0.3 * np.sin(2 * np.pi * 440.0 * t)).astype(np.float32)
    in_path = tmp_path / "in.wav"
    out_path = tmp_path / "out.wav"
    _write_wav(in_path, y, sr)

    # A deliberately partial map used to zero both unlisted quarters.
    apply_section_chains(str(in_path), str(out_path), [
        {"label": "chorus", "start_sec": 0.25, "end_sec": 0.75, "effects": []},
    ])

    out, _ = sf.read(str(out_path), dtype="float32")
    quarter = sr // 4
    np.testing.assert_allclose(out[:quarter], y[:quarter], atol=2e-7)
    np.testing.assert_allclose(out[3 * quarter:], y[3 * quarter:], atol=2e-7)


def test_apply_section_chains_rejects_overlapping_sections(tmp_path):
    sr = 44100
    in_path = tmp_path / "in.wav"
    _write_wav(in_path, np.zeros(sr, dtype=np.float32), sr)

    with pytest.raises(ValueError, match="ordered and non-overlapping"):
        apply_section_chains(str(in_path), str(tmp_path / "out.wav"), [
            {"start_sec": 0.0, "end_sec": 0.75, "effects": []},
            {"start_sec": 0.5, "end_sec": 1.0, "effects": []},
        ])
