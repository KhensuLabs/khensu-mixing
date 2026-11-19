import os
import numpy as np
import soundfile as sf
import pytest

from khensu_services.analysis.advanced_analyzer import analyze_advanced


@pytest.fixture
def short_wav(tmp_path):
    sr = 22050
    t = np.linspace(0, 3.0, int(sr * 3.0), endpoint=False)
    y = 0.2 * np.sin(2 * np.pi * 220.0 * t).astype(np.float32)
    path = os.path.join(tmp_path, "short.wav")
    sf.write(path, y, sr)
    return path


def test_progress_cb_fires_nine_times_in_order(short_wav):
    calls: list[tuple[str, int, int]] = []

    def cb(stage: str, idx: int, total: int) -> None:
        calls.append((stage, idx, total))

    analyze_advanced(short_wav, progress_cb=cb)

    assert len(calls) == 9
    expected_stages = [
        "loading_audio", "tempo_beats", "pitch_key", "sections",
        "section_loudness", "hooks", "memorability", "structural_flow",
        "heaviness",
    ]
    assert [c[0] for c in calls] == expected_stages
    assert [c[1] for c in calls] == list(range(1, 10))
    assert all(c[2] == 9 for c in calls)


def test_progress_cb_none_is_safe(short_wav):
    # No callback passed — must not raise.
    result = analyze_advanced(short_wav)
    assert result["mode"] == "advanced"
