"""Regenerate tests/shared-core/fixtures/native_meter_parity.json (CE-PLT-3).

The native C++ meters (shared-core NativeMeters) must agree with the Python
service's shared meters. This script synthesises a few deterministic signals,
measures them with the service's own functions (analysis/loudness.py,
analysis/dsp_metrics.py) and writes the signal recipes plus the reference
numbers. The C++ parity test rebuilds the same signals from the recipes and
compares (tolerance 0.05 LU / dB).

Run from the repository root with the service venv:

    local-services/.venv/Scripts/python scripts/gen_native_meter_fixtures.py

Re-run it (and commit the JSON) only when a Python meter changes on purpose.
"""
from __future__ import annotations

import json
import math
import sys
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "local-services"))

from khensu_services.analysis import dsp_metrics, loudness

OUT = ROOT / "tests" / "shared-core" / "fixtures" / "native_meter_parity.json"

# Deterministic generators shared with the C++ test (keep in lock-step with
# synthesize() in tests/shared-core/test_native_meters_parity.cpp).
_LCG_A = 1664525
_LCG_C = 1013904223


def lcg_noise(seed: int, n: int) -> np.ndarray:
    out = np.empty(n, dtype=np.float64)
    x = seed & 0xFFFFFFFF
    for i in range(n):
        x = (_LCG_A * x + _LCG_C) & 0xFFFFFFFF
        out[i] = (x / 4294967296.0) * 2.0 - 1.0
    return out


def synthesize(spec: dict) -> np.ndarray:
    """(channels, samples) float32 for one recipe."""
    sr = int(spec["sample_rate"])
    n = round(spec["duration_sec"] * sr)
    t = np.arange(n, dtype=np.float64) / sr
    chans = []
    for ch in spec["channels"]:
        y = np.zeros(n, dtype=np.float64)
        for tone in ch.get("tones", []):
            y += tone["amp"] * np.sin(2.0 * math.pi * tone["freq"] * t + tone.get("phase", 0.0))
        if "noise_seed" in ch:
            y += ch.get("noise_amp", 1.0) * lcg_noise(int(ch["noise_seed"]), n)
        # Piecewise gain steps: [[start_sec, gain], ...].
        steps = ch.get("gain_steps")
        if steps:
            g = np.ones(n, dtype=np.float64)
            for start, gain in steps:
                g[round(start * sr):] = gain
            y *= g
        chans.append(y.astype(np.float32))
    return np.stack(chans)


SIGNALS = [
    {"name": "sine_1k_-20dBFS_stereo_48k", "sample_rate": 48000, "duration_sec": 10.0,
     "channels": [{"tones": [{"freq": 1000.0, "amp": 0.1}]},
                  {"tones": [{"freq": 1000.0, "amp": 0.1}]}]},
    {"name": "sine_997_mono_44k1", "sample_rate": 44100, "duration_sec": 6.0,
     "channels": [{"tones": [{"freq": 997.0, "amp": 0.5}]}]},
    {"name": "intersample_peak_fs4_44k1", "sample_rate": 44100, "duration_sec": 2.0,
     "channels": [{"tones": [{"freq": 11025.0, "amp": 0.5, "phase": math.pi / 4}]},
                  {"tones": [{"freq": 11025.0, "amp": 0.5, "phase": math.pi / 4}]}]},
    {"name": "noise_level_steps_stereo_48k", "sample_rate": 48000, "duration_sec": 20.0,
     "channels": [{"noise_seed": 1, "noise_amp": 0.3,
                   "gain_steps": [[0.0, 0.1], [5.0, 0.3], [10.0, 1.0], [15.0, 0.05]]},
                  {"noise_seed": 2, "noise_amp": 0.3,
                   "gain_steps": [[0.0, 0.1], [5.0, 0.3], [10.0, 1.0], [15.0, 0.05]]}]},
    {"name": "partly_correlated_stereo_44k1", "sample_rate": 44100, "duration_sec": 8.0,
     "channels": [{"tones": [{"freq": 220.0, "amp": 0.3}, {"freq": 3300.0, "amp": 0.05}]},
                  {"tones": [{"freq": 220.0, "amp": 0.15}], "noise_seed": 7, "noise_amp": 0.1}]},
]


def octave_levels(mono: np.ndarray, sr: int) -> tuple[list[float], list[float]]:
    """spectral_tilt_db_per_octave's per-octave levels (the same Welch PSD and
    band rules); the slope itself comes from the real function below."""
    from scipy.signal import welch

    x = np.asarray(mono, dtype=np.float64)
    n_fft = max(4096, int(2 ** np.ceil(np.log2(sr / 10.0))))
    if x.shape[0] < n_fft:
        x = np.pad(x, (0, n_fft - x.shape[0]))
    freqs, psd = welch(x, fs=sr, window="hann", nperseg=n_fft, noverlap=n_fft // 2,
                       scaling="spectrum")
    cap = min(sr / 2.0, dsp_metrics.TILT_CAP_HZ)
    centers, levels = [], []
    for fc in dsp_metrics.OCTAVE_CENTERS_HZ:
        lo, hi = fc / np.sqrt(2.0), min(fc * np.sqrt(2.0), cap)
        if hi <= lo:
            continue
        covered = float(np.log2(hi / lo))
        if covered < 0.5:
            continue
        m = (freqs >= lo) & (freqs < hi)
        if not m.any():
            continue
        centers.append(fc)
        levels.append(10.0 * np.log10(float(psd[m].sum()) / covered + 1e-30))
    return centers, levels


def _finite(v):
    return None if v is None or not np.isfinite(v) else float(v)


def measure(spec: dict) -> dict:
    x = synthesize(spec)
    sr = int(spec["sample_rate"])
    st = loudness.short_term_loudness_series(x, sr, 3.0, 0.1)
    mom = loudness.short_term_loudness_series(x, sr, 0.4, 0.1)
    mono = x.mean(axis=0)
    centers, levels = octave_levels(mono, sr)
    out = {
        "integrated_lufs": _finite(loudness.integrated_lufs(x, sr)),
        "loudness_range_lu": _finite(loudness.loudness_range(x, sr)),
        "short_term_max_lufs": _finite(np.nanmax(st)) if np.isfinite(st).any() else None,
        "momentary_max_lufs": _finite(np.nanmax(mom)) if np.isfinite(mom).any() else None,
        "true_peak_dbtp": float(loudness.true_peak_dbtp(x, sr)),
        "sample_peak_dbfs": float(20.0 * np.log10(np.max(np.abs(x)) + 1e-12)),
        "octave_centers_hz": centers,
        "octave_levels_db": levels,
        "tilt_db_per_octave": float(dsp_metrics.spectral_tilt_db_per_octave(x, sr)),
    }
    if x.shape[0] >= 2:
        out["stereo_width"] = float(dsp_metrics.stereo_width(x[0], x[1]))
        out["stereo_correlation"] = float(dsp_metrics.stereo_correlation(x[0], x[1]))
    return out


def main() -> int:
    fixtures = {
        "generator": "scripts/gen_native_meter_fixtures.py",
        "tolerance": 0.05,
        "lcg": {"a": _LCG_A, "c": _LCG_C, "modulus": 4294967296},
        "signals": [{**spec, "expected": measure(spec)} for spec in SIGNALS],
    }
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(json.dumps(fixtures, indent=2) + "\n", encoding="utf-8")
    print(f"wrote {OUT.relative_to(ROOT)} ({len(SIGNALS)} signals)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
