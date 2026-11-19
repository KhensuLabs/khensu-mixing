"""The advanced /analyze response carries the waveform-symmetry fields the
C++ client parses (they were computed but dropped by the response model)."""
from khensu_services.analysis.models import AnalyzeResponse


def test_symmetry_fields_survive_the_response_model():
    payload = {
        "mode": "advanced", "bpm": 120.0, "bpm_confidence": 0.9, "beat_times": [],
        "downbeat_times": None, "key": "C major", "key_confidence": 0.5, "sections": [],
        "pitch_notes": None, "analysis_duration_ms": 1.0,
        "skewness": 0.4, "peak_asymmetry_ratio": 1.6, "is_asymmetric": True,
    }
    dumped = AnalyzeResponse(**payload).model_dump()
    assert dumped["skewness"] == 0.4
    assert dumped["peak_asymmetry_ratio"] == 1.6
    assert dumped["is_asymmetric"] is True


def test_symmetry_fields_default_when_absent():
    payload = {
        "mode": "quick", "bpm": 120.0, "bpm_confidence": 0.9, "beat_times": [],
        "downbeat_times": None, "key": "C major", "key_confidence": 0.5, "sections": [],
        "pitch_notes": None, "analysis_duration_ms": 1.0,
    }
    dumped = AnalyzeResponse(**payload).model_dump()
    assert dumped["peak_asymmetry_ratio"] == 1.0 and dumped["is_asymmetric"] is False
