import numpy as np
import soundfile as sf

from khensu_services.analysis.performance_analyzer import (
    analyze_groove_timing,
    analyze_harmony_timeline,
    analyze_stem_phase_correlation,
    estimate_reverb_tail,
)


def _write(path, audio, sr=22050):
    sf.write(path, audio, sr)
    return str(path)


def test_groove_and_phase_measurements(tmp_path):
    sr = 22050
    clicks = np.zeros(sr * 4)
    for sample in range(0, len(clicks), sr // 2):
        clicks[sample:sample + 80] = np.hanning(80)
    groove = analyze_groove_timing(_write(tmp_path / "clicks.wav", clicks, sr))
    assert groove["bpm"] > 80.0
    assert groove["pocket"] in {"on_grid", "ahead_of_grid", "behind_grid"}

    t = np.arange(sr) / sr
    first = _write(tmp_path / "first.wav", np.sin(2 * np.pi * 100 * t), sr)
    second = _write(tmp_path / "second.wav", -np.sin(2 * np.pi * 100 * t), sr)
    phase = analyze_stem_phase_correlation({"vocals": first, "bass": second})
    assert phase["pairs"][0]["polarity"] == "inverted"
    assert phase["pairs"][0]["correlation"] < -0.9


def test_reverb_tail_and_harmony_timeline(tmp_path):
    sr = 22050
    t = np.arange(sr * 3) / sr
    tail = np.exp(-t / 0.35) * np.sin(2 * np.pi * 400 * t)
    reverb = estimate_reverb_tail(_write(tmp_path / "tail.wav", tail, sr))
    assert reverb["rt60_sec"] is not None
    assert reverb["rt60_sec"] > 0.5

    c_major = sum(np.sin(2 * np.pi * freq * t) for freq in (261.63, 329.63, 392.0)) / 3
    harmony = analyze_harmony_timeline(_write(tmp_path / "c_major.wav", c_major, sr))
    assert harmony["progression"]
    assert harmony["progression"][0]["chord"] == "C"


def test_performance_endpoints(client, tmp_path):
    sr = 22050
    t = np.arange(sr) / sr
    path = _write(tmp_path / "tone.wav", np.sin(2 * np.pi * 220 * t), sr)
    assert client.post("/analyze/groove_timing", json={"file_path": path}).status_code == 200
    assert client.post("/analyze/reverb_tail", json={"file_path": path}).status_code == 200
    assert client.post("/analyze/harmony_timeline", json={"file_path": path}).status_code == 200
    response = client.post("/analyze/stem_phase", json={"stem_paths": {"a": path, "b": path}})
    assert response.status_code == 200
    assert response.json()["pairs"][0]["polarity"] == "aligned"
