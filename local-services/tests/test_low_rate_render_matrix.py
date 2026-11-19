"""Low-rate auxiliary render matrix (audit CQ-01).

The endpoint safety matrix covers the primary apply-chain route. These tests
exercise separate render helpers that also write audio, where a band above
Nyquist or an empty spectral slice must never produce non-finite output.
"""
from __future__ import annotations

import numpy as np
import pytest
import soundfile as sf


def _write_stereo(path, sr: int) -> str:
    t = np.arange(sr * 2) / sr
    mono = (0.18 * np.sin(2 * np.pi * 220.0 * t)
            + 0.05 * np.sin(2 * np.pi * 1200.0 * t)).astype(np.float32)
    sf.write(str(path), np.column_stack([mono, mono * 0.97]), sr)
    return str(path)


def _assert_finite_wav(path: str) -> None:
    audio, _ = sf.read(path, always_2d=True)
    assert audio.size > 0
    assert np.isfinite(audio).all()


@pytest.mark.parametrize("sr", [8000, 11025])
def test_auxiliary_render_paths_stay_finite_at_low_rates(tmp_path, sr):
    from khensu_services.analysis.dither_preview import analyze_dither
    from khensu_services.copilot.crossfeed import render_crossfeed_ab
    from khensu_services.copilot.device_sim import render_device_preview
    from khensu_services.copilot.dsp import apply_section_chains, render_delta
    from khensu_services.copilot.platform_render import render_platform_master

    source = _write_stereo(tmp_path / f"source-{sr}.wav", sr)

    delta = str(tmp_path / f"delta-{sr}.wav")
    render_delta(source, source, delta)
    _assert_finite_wav(delta)

    sections = [{"label": "full", "start_sec": 0.0, "end_sec": 2.0, "effects": []}]
    section_out = str(tmp_path / f"section-{sr}.wav")
    apply_section_chains(source, section_out, sections)
    _assert_finite_wav(section_out)

    platform_out = str(tmp_path / f"platform-{sr}.wav")
    render_platform_master(source, platform_out, target_lufs=-14.0, ceiling_dbtp=-1.0)
    _assert_finite_wav(platform_out)

    dither = analyze_dither(source, target_bits=16, out_dir=str(tmp_path / f"dither-{sr}"))
    _assert_finite_wav(dither["audition_path"])

    crossfeed = render_crossfeed_ab(source, str(tmp_path / f"crossfeed-{sr}"))
    _assert_finite_wav(crossfeed["headphone_path"])
    _assert_finite_wav(crossfeed["speaker_path"])

    device = render_device_preview(source, "phone")
    _assert_finite_wav(device["rendered_path"])
