"""Tests for the analysis enhancements: real EBU LRA, vocal analysis endpoint."""
from __future__ import annotations

import numpy as np
import pytest
import soundfile as sf


def test_ebu_lra_is_larger_for_dynamic_signal():
    from khensu_services.analysis.loudness import loudness_range
    sr = 44100

    # Steady tone -> ~0 LU range.
    steady = 0.3 * np.sin(2 * np.pi * 440 * np.arange(sr * 12) / sr)
    steady_st = np.stack([steady, steady])

    # Alternating loud/quiet 3 s blocks -> wide range.
    dyn = np.copy(steady)
    for i, start in enumerate(range(0, len(dyn), sr * 3)):
        gain = 1.0 if i % 2 == 0 else 0.1
        dyn[start:start + sr * 3] *= gain
    dyn_st = np.stack([dyn, dyn])

    lra_steady = loudness_range(steady_st, sr)
    lra_dyn = loudness_range(dyn_st, sr)

    assert lra_steady < 2.0
    assert lra_dyn > lra_steady + 3.0


def test_ebu_lra_short_signal_returns_zero():
    from khensu_services.analysis.loudness import loudness_range
    sr = 44100
    short = 0.3 * np.sin(2 * np.pi * 440 * np.arange(sr) / sr)  # 1 s
    assert loudness_range(np.stack([short, short]), sr) == 0.0


@pytest.fixture
def vocalish_wav(tmp_path):
    """Presence-band tone + periodic sibilant bursts + quiet breath gaps."""
    sr = 44100
    n = sr * 4
    t = np.arange(n) / sr
    voice = 0.3 * np.sin(2 * np.pi * 1500 * t)          # presence band
    # Sibilant bursts every second.
    sib = np.zeros(n)
    for onset in range(sr // 2, n, sr):
        seg = slice(onset, onset + sr // 8)
        sib[seg] += 0.25 * np.random.randn(sr // 8)
    y = (voice + sib).astype(np.float32)
    p = tmp_path / "vox.wav"
    sf.write(str(p), y, sr)
    return str(p)


def test_vocal_endpoint_returns_timeline_and_tiles(client, vocalish_wav):
    r = client.post("/analyze/vocal", json={"file_path": vocalish_wav})
    assert r.status_code == 200, r.text
    data = r.json()
    assert len(data["sibilance_timeline"]) > 5
    assert all(0.0 <= s["sibilance"] <= 1.0 for s in data["sibilance_timeline"])
    assert 0.0 <= data["presence_score"] <= 1.0
    assert "f2_800_1800" in data["formant_balance"]
    assert data["window_sec"] > 0.0


def test_vocal_endpoint_missing_file(client):
    assert client.post("/analyze/vocal", json={"file_path": "Q:/none.wav"}).status_code == 404


def test_vocal_sibilance_peaks_align_with_bursts(vocalish_wav):
    import librosa
    from khensu_services.analysis.vocal_analyzer import analyze_vocal
    y, sr = librosa.load(vocalish_wav, sr=None, mono=False)
    res = analyze_vocal(y, int(sr))
    sib = np.array([s["sibilance"] for s in res["sibilance_timeline"]])
    # The sibilant bursts should push the max sibilance well above the median.
    assert sib.max() > np.median(sib) + 0.1


def test_dominant_sibilance_hz_falls_within_sibilant_band(vocalish_wav):
    import librosa
    from khensu_services.analysis.vocal_analyzer import analyze_vocal
    y, sr = librosa.load(vocalish_wav, sr=None, mono=False)
    res = analyze_vocal(y, int(sr))
    # Broadband noise bursts in the fixture should produce a measured centroid
    # somewhere inside the 5-10 kHz sibilant band, not the unset 0.0 default.
    assert 5000.0 <= res["dominant_sibilance_hz"] <= 10000.0


def test_dominant_sibilance_hz_is_zero_without_sibilance():
    from khensu_services.analysis.vocal_analyzer import analyze_vocal
    sr = 44100
    t = np.arange(sr * 2) / sr
    # A clean low tone has no 5-10 kHz sibilant energy at all.
    y = (0.3 * np.sin(2 * np.pi * 200.0 * t)).astype(np.float32)
    res = analyze_vocal(y, sr)
    assert res["dominant_sibilance_hz"] == 0.0


def test_dominant_sibilance_hz_zero_for_short_clip():
    from khensu_services.analysis.vocal_analyzer import analyze_vocal
    sr = 44100
    y = np.zeros(100, dtype=np.float32)  # well under the sr//2 minimum
    res = analyze_vocal(y, sr)
    assert res["dominant_sibilance_hz"] == 0.0
