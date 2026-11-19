# local-services/tests/test_mid_side_spectrogram.py
"""Tests for the mid/side spectrogram."""
import numpy as np
import soundfile as sf

from khensu_services.analysis.mid_side_spectrogram import analyze_mid_side_spectrogram

SR = 44100


def _write(tmp_path, name, stereo):
    p = tmp_path / name
    sf.write(str(p), np.asarray(stereo, dtype=np.float32), SR)
    return str(p)


def test_side_energy_high_for_wide_content(tmp_path):
    t = np.arange(int(2.0 * SR)) / SR
    tone = 0.4 * np.sin(2 * np.pi * 2000 * t)
    # Hard anti-phase -> all energy in the side channel, none in mid.
    path = _write(tmp_path, "wide.wav", np.stack([tone, -tone], axis=1))
    r = analyze_mid_side_spectrogram(path, n_bands=24, target_frames=120)
    assert r["n_frames"] <= 120
    # Bin-less low bands are merged, so n_bands may shrink; edges stay consistent.
    assert 2 <= r["n_bands"] <= 24
    assert len(r["band_edges_hz"]) == r["n_bands"] + 1
    mid = np.array(r["mid_frames"])
    side = np.array(r["side_frames"])
    # Side carries the energy; mid is ~empty.
    assert side.max() > 0.5
    assert mid.mean() < side.mean()


def test_centered_content_lives_in_mid(tmp_path):
    t = np.arange(int(2.0 * SR)) / SR
    tone = 0.4 * np.sin(2 * np.pi * 500 * t)
    path = _write(tmp_path, "mono.wav", np.stack([tone, tone], axis=1))
    r = analyze_mid_side_spectrogram(path, n_bands=24, target_frames=120)
    mid = np.array(r["mid_frames"])
    side = np.array(r["side_frames"])
    assert mid.max() > 0.5
    assert mid.mean() > side.mean()


def test_frame_cap_and_shape(tmp_path):
    t = np.arange(int(6.0 * SR)) / SR
    m = 0.2 * np.sin(2 * np.pi * 300 * t)
    path = _write(tmp_path, "long.wav", np.stack([m, m * 0.9], axis=1))
    r = analyze_mid_side_spectrogram(path, n_bands=32, target_frames=200)
    assert r["n_frames"] <= 200
    assert all(len(row) == r["n_bands"] for row in r["mid_frames"])
