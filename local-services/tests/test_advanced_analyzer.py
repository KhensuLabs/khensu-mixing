"""Advanced analyzer tests on a synthetic 4/4 click track.

The analyzer no longer uses madmom (librosa beat tracking; basic-pitch is
optional and falls back to chroma), so nothing here is skipped for it - the
file used to skip wholesale on ``importorskip("madmom")``.
"""
import numpy as np
import soundfile as sf
import pytest


_CLICK_BPM = 100.0


@pytest.fixture(scope="module")
def tmp_click_wav(tmp_path_factory):
    """4/4 click track at 100 BPM for 16 bars (38.4 s), accented downbeats."""
    sr = 22050
    beat_interval = 60.0 / _CLICK_BPM  # 0.6 s
    duration = beat_interval * 64  # 16 bars * 4 beats
    y = np.zeros(int(sr * duration), dtype=np.float32)
    for beat_t in np.arange(0, duration, beat_interval):
        idx = int(round(beat_t * sr))
        end = min(idx + 256, len(y))
        amplitude = 0.9 if round(beat_t / beat_interval) % 4 == 0 else 0.5
        y[idx:end] += amplitude
    path = tmp_path_factory.mktemp("adv") / "click.wav"
    sf.write(str(path), y, sr)
    return str(path)


@pytest.fixture(scope="module")
def advanced_result(tmp_click_wav):
    from khensu_services.analysis.advanced_analyzer import analyze_advanced
    return analyze_advanced(tmp_click_wav)


def test_advanced_analyze_mode(advanced_result):
    result = advanced_result
    assert result["mode"] == "advanced"


def test_advanced_analyze_bpm_matches_the_click(advanced_result):
    result = advanced_result
    # The beat-fit tempo, not the quantised tempogram bin (AUDIT-015).
    assert result["bpm"] == pytest.approx(_CLICK_BPM, abs=0.5)


def test_advanced_analyze_downbeats_is_list(advanced_result):
    result = advanced_result
    assert isinstance(result["downbeat_times"], list)


def test_advanced_analyze_beat_times_is_list(advanced_result):
    result = advanced_result
    assert isinstance(result["beat_times"], list)
    assert all(isinstance(t, float) for t in result["beat_times"])


def test_advanced_analyze_key_is_valid(advanced_result):
    result = advanced_result
    assert isinstance(result["key"], str)
    assert "major" in result["key"] or "minor" in result["key"]


def test_advanced_analyze_pitch_notes_is_list(advanced_result):
    result = advanced_result
    assert isinstance(result["pitch_notes"], list)


def test_advanced_analyze_sections_non_empty(advanced_result):
    result = advanced_result
    assert len(result["sections"]) >= 1


def test_advanced_analyze_duration_ms_positive(advanced_result):
    result = advanced_result
    assert result["analysis_duration_ms"] > 0.0
