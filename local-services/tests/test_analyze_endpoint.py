import soundfile as sf
import numpy as np
import pytest
from unittest.mock import patch
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

_ADVANCED_RESULT = {
    **_QUICK_RESULT,
    "mode": "advanced",
    "bpm_confidence": 0.95,
    "downbeat_times": [0.5, 2.5, 4.5],
    "pitch_notes": [{"start_sec": 0.0, "end_sec": 0.5, "midi_pitch": 69, "velocity": 0.7}],
    "analysis_duration_ms": 15000.0,
    "hook_instances": [{"start_sec": 8.0, "end_sec": 16.0, "hook_type": "structural", "strength": 0.5}],
    "hook_strength": 0.5,
    "hook_density": 2.0,
    "hook_count": 1,
    "memorability_score": 0.55,
    "earworm_likelihood": 0.25,
    "melody_salience": 0.3,
    "arc_quality": 0.75,
    "heaviness_distortion": 0.2,
    "heaviness_weight": 0.15,
    "heaviness_clarity": 0.8,
    "heaviness_extremity": 0.175,
}


@pytest.fixture
def tmp_wav(tmp_path):
    path = tmp_path / "test.wav"
    sr = 22050
    y = (np.random.rand(sr * 3) * 0.1).astype(np.float32)
    sf.write(str(path), y, sr)
    return str(path)


def test_analyze_quick_returns_200(tmp_wav):
    with patch("khensu_services.analysis.router.analyze_quick", return_value=_QUICK_RESULT):
        resp = client.post("/analyze", json={"file_path": tmp_wav, "mode": "quick"})
    assert resp.status_code == 200


def test_analyze_quick_response_body(tmp_wav):
    with patch("khensu_services.analysis.router.analyze_quick", return_value=_QUICK_RESULT):
        resp = client.post("/analyze", json={"file_path": tmp_wav, "mode": "quick"})
    data = resp.json()
    assert data["mode"] == "quick"
    assert data["bpm"] == 120.0
    assert data["downbeat_times"] is None
    assert data["pitch_notes"] is None


def test_analyze_default_mode_calls_quick(tmp_wav):
    with patch("khensu_services.analysis.router.analyze_quick", return_value=_QUICK_RESULT) as mock_q:
        client.post("/analyze", json={"file_path": tmp_wav})
    mock_q.assert_called_once_with(tmp_wav)


def test_analyze_advanced_returns_200(tmp_wav):
    with patch("khensu_services.analysis.router.analyze_advanced", return_value=_ADVANCED_RESULT):
        resp = client.post("/analyze", json={"file_path": tmp_wav, "mode": "advanced"})
    assert resp.status_code == 200


def test_analyze_advanced_response_body(tmp_wav):
    with patch("khensu_services.analysis.router.analyze_advanced", return_value=_ADVANCED_RESULT):
        resp = client.post("/analyze", json={"file_path": tmp_wav, "mode": "advanced"})
    data = resp.json()
    assert data["mode"] == "advanced"
    assert data["downbeat_times"] == [0.5, 2.5, 4.5]
    assert len(data["pitch_notes"]) == 1
    assert data["pitch_notes"][0]["midi_pitch"] == 69


def test_analyze_missing_file_returns_404():
    # Missing input file is 404 everywhere (common/errors.py convention).
    resp = client.post("/analyze", json={"file_path": "C:/does/not/exist.wav", "mode": "quick"})
    assert resp.status_code == 404
    assert "not found" in resp.json()["detail"].lower()


def test_analyze_invalid_mode_returns_422(tmp_wav):
    resp = client.post("/analyze", json={"file_path": tmp_wav, "mode": "turbo"})
    assert resp.status_code == 422


def test_analyze_missing_body_returns_422():
    resp = client.post("/analyze", json={})
    assert resp.status_code == 422


def test_analyze_analyzer_error_returns_500(tmp_wav):
    with patch(
        "khensu_services.analysis.router.analyze_quick",
        side_effect=RuntimeError("librosa exploded"),
    ):
        resp = client.post("/analyze", json={"file_path": tmp_wav, "mode": "quick"})
    assert resp.status_code == 500


def test_analyze_advanced_sp06_fields_present(tmp_wav):
    with patch("khensu_services.analysis.router.analyze_advanced", return_value=_ADVANCED_RESULT):
        resp = client.post("/analyze", json={"file_path": tmp_wav, "mode": "advanced"})
    data = resp.json()
    assert isinstance(data["hook_strength"], float)
    assert isinstance(data["memorability_score"], float)
    assert isinstance(data["heaviness_distortion"], float)
    assert data["hook_count"] == 1
    assert len(data["hook_instances"]) == 1
    assert data["hook_instances"][0]["hook_type"] == "structural"
