"""Tests for per-section LUFS computation in advanced_analyzer."""
import numpy as np
from khensu_services.analysis.advanced_analyzer import _add_section_lufs


def _sine(sr: int, duration_sec: float, freq_hz: float = 440.0) -> np.ndarray:
    t = np.linspace(0.0, duration_sec, int(sr * duration_sec), endpoint=False)
    return (np.sin(2.0 * np.pi * freq_hz * t) * 0.5).astype(np.float32)


def test_add_section_lufs_populates_key():
    sr = 44100
    y  = _sine(sr, 10.0)
    sections = [{"start_sec": 0.0, "label": "A"}, {"start_sec": 5.0, "label": "B"}]
    _add_section_lufs(sections, y, sr, 10.0)
    assert "lufs_integrated" in sections[0]
    assert "lufs_integrated" in sections[1]


def test_add_section_lufs_returns_float():
    sr = 44100
    y  = _sine(sr, 10.0)
    sections = [{"start_sec": 0.0, "label": "A"}]
    _add_section_lufs(sections, y, sr, 10.0)
    assert isinstance(sections[0]["lufs_integrated"], float)


def test_short_section_is_not_measured():
    sr = 44100
    y  = _sine(sr, 2.0)   # total 2s — section < 4s threshold
    sections = [{"start_sec": 0.0, "label": "A"}]
    _add_section_lufs(sections, y, sr, 2.0)
    # DSP-12: a too-short section is "not measured" (None), not silence -
    # a loud 3 s break used to read -70 LUFS. (The old 0.0 sentinel meant
    # "extremely loud".)
    assert sections[0]["lufs_integrated"] is None


def test_silent_long_section_reads_silence_floor():
    sr = 44100
    y = np.zeros(int(sr * 6.0), dtype=np.float32)
    sections = [{"start_sec": 0.0, "label": "A"}]
    _add_section_lufs(sections, y, sr, 6.0)
    assert sections[0]["lufs_integrated"] == -70.0


def test_lufs_is_negative():
    sr = 44100
    y  = _sine(sr, 10.0)
    sections = [{"start_sec": 0.0, "label": "A"}]
    _add_section_lufs(sections, y, sr, 10.0)
    assert sections[0]["lufs_integrated"] < 0.0
