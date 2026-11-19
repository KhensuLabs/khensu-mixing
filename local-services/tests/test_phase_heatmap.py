# local-services/tests/test_phase_heatmap.py
"""Tests for the full-spectrum phase-correlation heatmap."""
import numpy as np
import soundfile as sf

from khensu_services.analysis.phase_heatmap import analyze_phase_heatmap

SR = 44100


def _write(tmp_path, name, stereo):
    p = tmp_path / name
    sf.write(str(p), np.asarray(stereo, dtype=np.float32), SR)
    return str(p)


def _band_index(edges, hz):
    for i in range(len(edges) - 1):
        if edges[i] <= hz < edges[i + 1]:
            return i
    return len(edges) - 2


def test_antiphase_band_reads_negative(tmp_path):
    t = np.arange(int(2.5 * SR)) / SR
    lowmid = 0.4 * np.sin(2 * np.pi * 300 * t)
    high = 0.3 * np.sin(2 * np.pi * 5000 * t)
    left = lowmid + high
    right = -lowmid + high          # 300 Hz anti-phase, 5 kHz correlated
    r = analyze_phase_heatmap(_write(tmp_path, "anti.wav", np.stack([left, right], axis=1)))
    grid = np.array(r["grid"], dtype=float)   # None (no energy) -> nan
    edges = r["band_edges_hz"]
    lo_i = _band_index(edges, 300.0)
    hi_i = _band_index(edges, 5000.0)
    assert grid[:, lo_i].mean() < -0.5      # cancels in mono
    assert grid[:, hi_i].mean() > 0.5       # mono-safe
    assert -1.0 <= np.nanmin(grid) and np.nanmax(grid) <= 1.0


def test_correlated_mix_all_positive(tmp_path):
    t = np.arange(int(2.0 * SR)) / SR
    m = 0.3 * np.sin(2 * np.pi * 200 * t) + 0.2 * np.sin(2 * np.pi * 3000 * t)
    r = analyze_phase_heatmap(_write(tmp_path, "corr.wav", np.stack([m, m], axis=1)))
    grid = np.array(r["grid"], dtype=float)
    active = grid[np.isfinite(grid) & (np.abs(grid) > 1e-3)]
    assert active.mean() > 0.8


def test_mono_file_empty_grid(tmp_path):
    t = np.arange(int(1.5 * SR)) / SR
    m = 0.3 * np.sin(2 * np.pi * 400 * t)
    r = analyze_phase_heatmap(_write(tmp_path, "mono.wav", np.stack([m], axis=1)))
    assert r["is_stereo"] is False
    assert r["grid"] == []


def test_silent_cells_are_none_not_decorrelated(tmp_path):
    # DSP-16: digital silence used to read 0.0, i.e. "decorrelated".
    t = np.arange(int(2.0 * SR)) / SR
    m = 0.3 * np.sin(2 * np.pi * 1000 * t)
    m[: SR] = 0.0                                   # first second silent
    r = analyze_phase_heatmap(_write(tmp_path, "gap.wav", np.stack([m, m], axis=1)))
    assert r["grid"][0] and all(v is None for v in r["grid"][0])
    assert any(v is not None for v in r["grid"][-1])


def test_short_antiphase_event_survives_pooling(tmp_path):
    # DSP-16: with more frames than columns every frame is pooled, so a
    # 0.25 s anti-phase burst in a 60 s correlated track is not skipped.
    n = 60 * SR
    t = np.arange(n) / SR
    left = 0.3 * np.sin(2 * np.pi * 300 * t)
    right = left.copy()
    s0 = 31 * SR + 1234
    right[s0:s0 + SR // 4] *= -1.0
    r = analyze_phase_heatmap(_write(tmp_path, "burst.wav", np.stack([left, right], axis=1)),
                              target_frames=40)
    grid = np.array(r["grid"], dtype=float)
    lo_i = _band_index(r["band_edges_hz"], 300.0)
    assert r["n_frames"] == 40
    assert np.nanmin(grid[:, lo_i]) < 0.9           # the burst pulls its column down
