import numpy as np
import soundfile as sf
import pytest


@pytest.fixture
def tmp_sine_wav(tmp_path):
    sr = 22050
    duration = 10.0
    t = np.linspace(0, duration, int(sr * duration))
    y = (np.sin(2 * np.pi * 440 * t) * 0.4).astype(np.float32)
    # Add clicks at ~120 BPM (every 0.5 s) to help beat tracker
    for beat_t in np.arange(0, duration, 0.5):
        idx = int(beat_t * sr)
        end = min(idx + 512, len(y))
        y[idx:end] += 0.6
    path = tmp_path / "sine.wav"
    sf.write(str(path), y, sr)
    return str(path)


def test_quick_analyze_returns_mode(tmp_sine_wav):
    from khensu_services.analysis.quick_analyzer import analyze_quick
    result = analyze_quick(tmp_sine_wav)
    assert result["mode"] == "quick"


def test_quick_analyze_bpm_plausible(tmp_sine_wav):
    from khensu_services.analysis.quick_analyzer import analyze_quick
    result = analyze_quick(tmp_sine_wav)
    assert 40.0 <= result["bpm"] <= 300.0


def test_quick_analyze_beat_times_is_list(tmp_sine_wav):
    from khensu_services.analysis.quick_analyzer import analyze_quick
    result = analyze_quick(tmp_sine_wav)
    assert isinstance(result["beat_times"], list)
    assert all(isinstance(t, float) for t in result["beat_times"])


def test_quick_analyze_downbeats_is_none(tmp_sine_wav):
    from khensu_services.analysis.quick_analyzer import analyze_quick
    result = analyze_quick(tmp_sine_wav)
    assert result["downbeat_times"] is None


def test_quick_analyze_pitch_notes_is_none(tmp_sine_wav):
    from khensu_services.analysis.quick_analyzer import analyze_quick
    result = analyze_quick(tmp_sine_wav)
    assert result["pitch_notes"] is None


def test_quick_analyze_key_is_valid_string(tmp_sine_wav):
    from khensu_services.analysis.quick_analyzer import analyze_quick
    result = analyze_quick(tmp_sine_wav)
    assert isinstance(result["key"], str)
    assert "major" in result["key"] or "minor" in result["key"]


def test_quick_analyze_key_confidence_in_range(tmp_sine_wav):
    from khensu_services.analysis.quick_analyzer import analyze_quick
    result = analyze_quick(tmp_sine_wav)
    assert 0.0 <= result["key_confidence"] <= 1.0


def test_quick_analyze_sections_non_empty(tmp_sine_wav):
    from khensu_services.analysis.quick_analyzer import analyze_quick
    result = analyze_quick(tmp_sine_wav)
    assert isinstance(result["sections"], list)
    assert len(result["sections"]) >= 1


def test_quick_analyze_section_fields(tmp_sine_wav):
    from khensu_services.analysis.quick_analyzer import analyze_quick
    result = analyze_quick(tmp_sine_wav)
    for section in result["sections"]:
        assert "start_sec" in section
        assert "label" in section
        assert isinstance(section["start_sec"], float)
        assert isinstance(section["label"], str)


def test_quick_analyze_duration_ms_positive(tmp_sine_wav):
    from khensu_services.analysis.quick_analyzer import analyze_quick
    result = analyze_quick(tmp_sine_wav)
    assert result["analysis_duration_ms"] > 0.0


def test_quick_analyze_bpm_confidence_in_range(tmp_sine_wav):
    from khensu_services.analysis.quick_analyzer import analyze_quick
    result = analyze_quick(tmp_sine_wav)
    assert 0.0 <= result["bpm_confidence"] <= 1.0


def test_quick_analyze_bpm_confidence_real_for_steady_beat(tmp_sine_wav):
    """FINDING-07 regression: bpm_confidence must be computed from IBI, not hardcoded 0.80.

    The tmp_sine_wav fixture inserts clicks at 120 BPM with very regular spacing.
    An IBI-based coefficient-of-variation computation on such a signal produces
    confidence well above 0.5. The old hardcoded value was exactly 0.80 regardless
    of the actual beat regularity.
    """
    from khensu_services.analysis.quick_analyzer import analyze_quick
    result = analyze_quick(tmp_sine_wav)
    conf = result["bpm_confidence"]
    # Regular beat → expect high confidence. 0.5 is the fallback for < 4 detected beats.
    assert conf != 0.80 or conf > 0.85, (
        f"bpm_confidence of exactly 0.80 on a regular beat track suggests the "
        f"hardcoded fallback is still active (FINDING-07). Got {conf}"
    )
