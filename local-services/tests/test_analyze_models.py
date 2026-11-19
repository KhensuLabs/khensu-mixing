import pytest
from pydantic import ValidationError
from khensu_services.analysis.models import (
    AnalyzeRequest,
    AnalyzeResponse,
    SectionMarker,
    PitchNote,
)

# SP-06 additions
from khensu_services.analysis.models import HookInstance


def test_request_defaults_to_quick():
    req = AnalyzeRequest(file_path="C:/audio/track.wav")
    assert req.mode == "quick"


def test_request_accepts_advanced():
    req = AnalyzeRequest(file_path="C:/audio/track.wav", mode="advanced")
    assert req.mode == "advanced"


def test_request_rejects_invalid_mode():
    with pytest.raises(ValidationError):
        AnalyzeRequest(file_path="track.wav", mode="turbo")


def test_section_marker_fields():
    s = SectionMarker(start_sec=0.0, label="A")
    assert s.start_sec == 0.0
    assert s.label == "A"


def test_pitch_note_fields():
    n = PitchNote(start_sec=1.0, end_sec=1.5, midi_pitch=60, velocity=0.8)
    assert n.midi_pitch == 60


def test_response_quick_shape():
    resp = AnalyzeResponse(
        mode="quick",
        bpm=128.0,
        bpm_confidence=0.8,
        beat_times=[0.5, 1.0, 1.5],
        downbeat_times=None,
        key="C minor",
        key_confidence=0.72,
        sections=[SectionMarker(start_sec=0.0, label="A")],
        pitch_notes=None,
        analysis_duration_ms=500.0,
    )
    assert resp.bpm == 128.0
    assert resp.downbeat_times is None
    assert resp.pitch_notes is None


def test_response_advanced_shape():
    resp = AnalyzeResponse(
        mode="advanced",
        bpm=120.0,
        bpm_confidence=0.95,
        beat_times=[0.5, 1.0],
        downbeat_times=[0.5, 2.5],
        key="A minor",
        key_confidence=0.88,
        sections=[SectionMarker(start_sec=0.0, label="A")],
        pitch_notes=[PitchNote(start_sec=0.0, end_sec=0.5, midi_pitch=69, velocity=0.7)],
        analysis_duration_ms=12000.0,
    )
    assert resp.downbeat_times == [0.5, 2.5]
    assert len(resp.pitch_notes) == 1


def test_response_rejects_invalid_mode():
    with pytest.raises(ValidationError):
        AnalyzeResponse(
            mode="turbo",
            bpm=120.0,
            bpm_confidence=0.9,
            beat_times=[],
            downbeat_times=None,
            key="C major",
            key_confidence=0.5,
            sections=[],
            pitch_notes=None,
            analysis_duration_ms=0.0,
        )


def test_analyze_response_sp06_fields_optional():
    resp = AnalyzeResponse(
        mode="quick",
        bpm=120.0,
        bpm_confidence=0.8,
        beat_times=[0.5, 1.0],
        downbeat_times=None,
        key="A minor",
        key_confidence=0.7,
        sections=[SectionMarker(start_sec=0.0, label="A")],
        pitch_notes=None,
        analysis_duration_ms=450.0,
    )
    assert resp.hook_instances is None
    assert resp.memorability_score is None
    assert resp.heaviness_distortion is None


def test_hook_instance_invalid_type_raises():
    with pytest.raises(ValidationError):
        HookInstance(start_sec=0.0, end_sec=1.0, hook_type="unknown", strength=0.5)
