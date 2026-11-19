from __future__ import annotations
import numpy as np
import pytest


@pytest.fixture
def simple_stereo_audio():
    """4-second stereo tone at 440 Hz."""
    sr = 22050
    t = np.linspace(0, 4.0, 4 * sr)
    mono = 0.5 * np.sin(2 * np.pi * 440 * t)
    return np.stack([mono, mono]), sr, [{"start_sec": 0.0, "label": "A"}]


def test_analyze_sections_returns_list(simple_stereo_audio):
    from khensu_services.analysis.section_analyzer import analyze_sections
    y, sr, sections = simple_stereo_audio
    result = analyze_sections(y, sr, sections, total_duration_sec=4.0)
    assert isinstance(result, list)
    assert len(result) == 1
    s = result[0]
    assert s["label"] == "A"
    assert s["duration_sec"] >= 4.0


def test_analyze_sections_computes_lufs(simple_stereo_audio):
    from khensu_services.analysis.section_analyzer import analyze_sections
    y, sr, sections = simple_stereo_audio
    result = analyze_sections(y, sr, sections, total_duration_sec=4.0)
    assert "lufs" in result[0]
    assert result[0]["lufs"] < 0.0, "LUFS should be negative"


def test_analyze_sections_skips_short_sections():
    from khensu_services.analysis.section_analyzer import analyze_sections
    sr = 22050
    y = np.zeros((2, sr))  # 1 second only
    sections = [{"start_sec": 0.0, "label": "A"}]
    result = analyze_sections(y, sr, sections, total_duration_sec=1.0)
    # Section under 4 seconds should still be returned but may have None lufs
    assert len(result) == 1
    # Short sections get lufs=None
    assert result[0]["lufs"] is None
