"""Tests for the graph processing schema (parallel / sidechain / multiband)."""
from __future__ import annotations

import numpy as np
import pytest
import soundfile as sf

from khensu_services.copilot.dsp import (
    apply_graph, _apply_sidechain_compressor, _band_split,
)


@pytest.fixture
def drum_like(tmp_path):
    """Sub-bass drone + a train of sharp transients (broadband)."""
    sr = 44100
    n = sr * 2
    t = np.arange(n) / sr
    drone = 0.3 * np.sin(2 * np.pi * 60.0 * t)
    transient = np.zeros(n)
    for onset in range(0, n, sr // 4):
        transient[onset:onset + 200] += np.hanning(200) * 0.8
    y = (drone + transient).astype(np.float32)
    path = tmp_path / "drums.wav"
    sf.write(str(path), y, sr)
    return str(path), y, sr


def test_graph_flat_form_matches_effect_list(drum_like, tmp_path):
    src, _, _ = drum_like
    out = str(tmp_path / "flat.wav")
    res = apply_graph(src, out, {"effects": [{"effect": "Gain", "parameters": {"gain_db": -6.0}}]})
    assert res["applied"] == ["b0:Gain"]
    proc, _ = sf.read(out)
    orig, _ = sf.read(src)
    assert np.max(np.abs(proc)) < np.max(np.abs(orig))  # attenuated


def test_parallel_compression_preserves_more_peak_than_serial(drum_like, tmp_path):
    src, y, sr = drum_like
    heavy = {"effect": "Compressor",
             "parameters": {"threshold_db": -30.0, "ratio": 10.0, "attack_ms": 1.0, "release_ms": 80.0}}

    # Serial: fully crushed.
    serial_out = str(tmp_path / "serial.wav")
    apply_graph(src, serial_out, {"effects": [heavy]})
    serial, _ = sf.read(serial_out)

    # Parallel: crushed branch blended 50% under a clean branch.
    parallel_out = str(tmp_path / "parallel.wav")
    apply_graph(src, parallel_out, {"branches": [
        {"effects": [], "mix": 0.5},
        {"effects": [heavy], "mix": 0.5},
    ]})
    parallel, _ = sf.read(parallel_out)

    def crest(x):
        peak = np.max(np.abs(x))
        rms = np.sqrt(np.mean(x ** 2)) + 1e-12
        return peak / rms

    # Parallel compression retains more transient (higher crest) than fully
    # serial crushing of the same signal.
    assert crest(parallel) > crest(serial)


def test_sidechain_hpf_reduces_low_end_over_triggering(drum_like):
    _, y, sr = drum_like
    x = np.stack([y, y]).astype(np.float64)

    broadband = _apply_sidechain_compressor(x, sr, threshold_db=-24.0, ratio=8.0,
                                            attack_ms=5.0, release_ms=100.0,
                                            sidechain_hpf_hz=0.0)
    hpf_keyed = _apply_sidechain_compressor(x, sr, threshold_db=-24.0, ratio=8.0,
                                            attack_ms=5.0, release_ms=100.0,
                                            sidechain_hpf_hz=120.0)

    # With the detector high-passed, the sub-bass drone triggers far less gain
    # reduction, so more low-end energy survives.
    def low_energy(sig):
        band = _band_split(sig, sr, 0.0, 120.0)
        return float(np.sum(band ** 2))

    assert low_energy(hpf_keyed) > low_energy(broadband)


def test_multiband_branches_sum_without_severe_comb(drum_like, tmp_path):
    src, y, sr = drum_like
    out = str(tmp_path / "mb.wav")
    # Three bands, each pass-through (no compression) — summing the zero-phase
    # splits must reconstruct close to the original (no deep comb notches).
    res = apply_graph(src, out, {"branches": [
        {"effects": [], "band_lo_hz": 0.0,   "band_hi_hz": 200.0},
        {"effects": [], "band_lo_hz": 200.0, "band_hi_hz": 4000.0},
        {"effects": [], "band_lo_hz": 4000.0,"band_hi_hz": 0.0},
    ]})
    assert not res["skipped"]
    proc, _ = sf.read(out)
    orig, _ = sf.read(src)
    n = min(len(proc), len(orig))
    # Reconstruction should retain most of the energy (bands overlap slightly at
    # crossovers but must not cancel).
    ratio = np.sqrt(np.mean(proc[:n] ** 2)) / (np.sqrt(np.mean(orig[:n] ** 2)) + 1e-12)
    assert 0.7 < ratio < 1.4


def test_graph_endpoint_roundtrip(client, drum_like, tmp_path):
    src, _, _ = drum_like
    out = str(tmp_path / "ep.wav")
    r = client.post("/copilot/apply_chain", json={
        "file_path": src, "output_path": out, "effects": [],
        "graph": {
            "branches": [
                {"effects": [], "mix": 0.5},
                {"effects": [{"effect": "SidechainCompressor",
                              "parameters": {"threshold_db": -20.0, "ratio": 6.0,
                                             "sidechain_hpf_hz": 100.0}}], "mix": 0.5},
            ],
            "post": [{"effect": "Limiter", "parameters": {"threshold_db": -1.0}}],
        },
    })
    assert r.status_code == 200, r.text
    body = r.json()
    assert any("SidechainCompressor" in a for a in body["applied_effects"])
    assert any("post:Limiter" in a for a in body["applied_effects"])
