from __future__ import annotations
import numpy as np
import pytest
from unittest.mock import patch, MagicMock
from fastapi.testclient import TestClient
from khensu_services.server import app

client = TestClient(app)

_QUICK_RESULT = {
    "mode": "quick",
    "bpm": 120.0,
    "bpm_confidence": 0.80,
    "beat_times": [0.5, 1.0, 1.5, 2.0],
    "downbeat_times": None,
    "key": "A minor",
    "key_confidence": 0.72,
    "sections": [{"start_sec": 0.0, "label": "A"}],
    "pitch_notes": None,
    "analysis_duration_ms": 450.0,
}

_SECTION_RESULT = [
    {
        "label": "A",
        "start_sec": 0.0,
        "duration_sec": 10.0,
        "lufs": -14.5,
        "crest_db": 12.0,
        "lra": 6.0,
        "stereo_correlation": 0.9,
        "balance_low": 0.35,
        "dominant_freq_hz": 440.0,
    }
]

_DUMMY_AUDIO = np.zeros((2, 22050 * 10), dtype=np.float32)
_FAKE_PATH = "/fake/audio/track.wav"


def _mocked_path_exists(path_str: str) -> bool:
    return path_str == _FAKE_PATH


def _make_path_mock():
    """Return a mock Path that reports exists() == True for our fake path."""
    m = MagicMock()
    m.exists.return_value = True
    return m


def test_sections_endpoint_returns_200():
    with (
        patch("pathlib.Path.exists", return_value=True),
        patch("khensu_services.common.errors.validate_audio_decode_budget"),
        patch("khensu_services.analysis.quick_analyzer.analyze_quick", return_value=_QUICK_RESULT),
        patch("khensu_services.analysis.router.analyze_sections", return_value=_SECTION_RESULT),
        patch("librosa.load", return_value=(_DUMMY_AUDIO, 22050)),
    ):
        resp = client.post("/analyze/sections", json={"file_path": _FAKE_PATH})
    assert resp.status_code == 200, resp.text


def test_sections_endpoint_has_section_analysis_field():
    with (
        patch("pathlib.Path.exists", return_value=True),
        patch("khensu_services.common.errors.validate_audio_decode_budget"),
        patch("khensu_services.analysis.quick_analyzer.analyze_quick", return_value=_QUICK_RESULT),
        patch("khensu_services.analysis.router.analyze_sections", return_value=_SECTION_RESULT),
        patch("librosa.load", return_value=(_DUMMY_AUDIO, 22050)),
    ):
        resp = client.post("/analyze/sections", json={"file_path": _FAKE_PATH})
    data = resp.json()
    assert "section_analysis" in data
    assert isinstance(data["section_analysis"], list)
    assert len(data["section_analysis"]) == 1


def test_sections_endpoint_section_has_required_fields():
    with (
        patch("pathlib.Path.exists", return_value=True),
        patch("khensu_services.common.errors.validate_audio_decode_budget"),
        patch("khensu_services.analysis.quick_analyzer.analyze_quick", return_value=_QUICK_RESULT),
        patch("khensu_services.analysis.router.analyze_sections", return_value=_SECTION_RESULT),
        patch("librosa.load", return_value=(_DUMMY_AUDIO, 22050)),
    ):
        resp = client.post("/analyze/sections", json={"file_path": _FAKE_PATH})
    s = resp.json()["section_analysis"][0]
    assert s["label"] == "A"
    assert s["start_sec"] == pytest.approx(0.0)
    assert s["duration_sec"] == pytest.approx(10.0)
    assert s["lufs"] == pytest.approx(-14.5)


def test_sections_endpoint_also_has_base_fields():
    with (
        patch("pathlib.Path.exists", return_value=True),
        patch("khensu_services.common.errors.validate_audio_decode_budget"),
        patch("khensu_services.analysis.quick_analyzer.analyze_quick", return_value=_QUICK_RESULT),
        patch("khensu_services.analysis.router.analyze_sections", return_value=_SECTION_RESULT),
        patch("librosa.load", return_value=(_DUMMY_AUDIO, 22050)),
    ):
        resp = client.post("/analyze/sections", json={"file_path": _FAKE_PATH})
    data = resp.json()
    assert data["bpm"] == pytest.approx(120.0)
    assert data["key"] == "A minor"


def test_sections_endpoint_rejects_missing_file():
    resp = client.post("/analyze/sections", json={"file_path": "/nonexistent/path/file.wav"})
    assert resp.status_code == 404, resp.text
