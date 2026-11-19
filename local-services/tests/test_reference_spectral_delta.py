# local-services/tests/test_reference_spectral_delta.py
"""Tests for the time-varying reference spectral-delta."""
import numpy as np
import soundfile as sf

from khensu_services.analysis.reference_spectral_delta import analyze_reference_spectral_delta

SR = 44100


def _write(tmp_path, name, mono):
    p = tmp_path / name
    sf.write(str(p), np.asarray(mono, dtype=np.float32), SR)
    return str(p)


def _band_index(edges, hz):
    for i in range(len(edges) - 1):
        if edges[i] <= hz < edges[i + 1]:
            return i
    return len(edges) - 2


def test_bright_target_reads_hot_in_highs(tmp_path):
    rng = np.random.default_rng(0)
    t = np.arange(int(3.0 * SR)) / SR
    # Reference: mostly low-mid energy. Target: same plus a strong 8 kHz tone.
    ref = 0.3 * np.sin(2 * np.pi * 300 * t) + 0.05 * rng.standard_normal(len(t))
    tgt = 0.3 * np.sin(2 * np.pi * 300 * t) + 0.4 * np.sin(2 * np.pi * 8000 * t)
    r = analyze_reference_spectral_delta(_write(tmp_path, "t.wav", tgt),
                                         _write(tmp_path, "r.wav", ref))
    grid = np.array(r["grid"])
    hi = _band_index(r["band_edges_hz"], 8000.0)
    lo = _band_index(r["band_edges_hz"], 300.0)
    # The target is much hotter than the reference in the highs (positive delta).
    assert grid[:, hi].mean() > grid[:, lo].mean() + 3.0


def test_identical_target_and_reference_near_zero(tmp_path):
    t = np.arange(int(2.0 * SR)) / SR
    m = 0.3 * np.sin(2 * np.pi * 500 * t) + 0.2 * np.sin(2 * np.pi * 2500 * t)
    path = _write(tmp_path, "same.wav", m)
    r = analyze_reference_spectral_delta(path, path)
    grid = np.array(r["grid"])
    # Same file vs itself -> tonal-balance delta hovers near zero on average.
    assert abs(float(grid.mean())) < 1.0


def _pink(n, seed=3):
    rng = np.random.default_rng(seed)
    spec = np.fft.rfft(rng.standard_normal(n))
    f = np.arange(spec.size, dtype=float)
    f[0] = 1.0
    x = np.fft.irfft(spec / np.sqrt(f), n)
    return 0.3 * x / np.max(np.abs(x))


def _pink_at_rates(tmp_path, rates=(44100, 48000, 96000), dur=8.0):
    from scipy.signal import resample_poly
    base = _pink(int(dur * 96000))
    paths = {}
    for sr in rates:
        g = np.gcd(sr, 96000)
        y = base if sr == 96000 else resample_poly(base, sr // g, 96000 // g)
        p = tmp_path / f"pink_{sr}.wav"
        sf.write(str(p), y.astype(np.float32), sr, subtype="FLOAT")
        paths[sr] = str(p)
    return paths


def test_identical_content_across_rates_has_no_delta(tmp_path):
    # AUDIT-007: one shared band grid and dB-of-mean-power on both sides, so
    # the same content at 44.1 / 48 / 96 kHz reads flat in every band.
    from khensu_services.analysis.reference_spectral_delta import average_tonal_delta
    paths = _pink_at_rates(tmp_path)
    for tgt_sr in paths:
        for ref_sr in paths:
            d = average_tonal_delta(paths[tgt_sr], paths[ref_sr])
            assert d["band_edges_hz"][-1] <= 20000.0
            worst = max(abs(v) for v in d["delta_db"])
            assert worst < 0.3, (tgt_sr, ref_sr, worst)


def test_grid_edges_are_shared_and_capped(tmp_path):
    paths = _pink_at_rates(tmp_path, rates=(44100, 96000), dur=3.0)
    r = analyze_reference_spectral_delta(paths[96000], paths[44100])
    assert r["band_edges_hz"][-1] <= 20000.0
    assert r["n_bands"] == len(r["band_edges_hz"]) - 1
    assert min(min(row) for row in r["grid"]) > -60.0   # no empty -120 dB bands
