from khensu_services.analysis.separator import _demucs_progress_fraction


def test_progress_fraction_mid_run():
    info = {"state": "running", "segment_offset": 220500, "audio_length": 441000}
    assert _demucs_progress_fraction(info) == 0.5


def test_progress_fraction_end_state():
    info = {"state": "end", "segment_offset": 999, "audio_length": 441000}
    assert _demucs_progress_fraction(info) == 1.0


def test_progress_fraction_missing_audio_length_is_safe():
    info = {"state": "running", "segment_offset": 100}
    assert _demucs_progress_fraction(info) == 0.0
