"""Regenerate tests/shared-core/fixtures/render_parity.json (OVH-9).

The desktop renders a fix natively (shared-core OfflineChainRenderer) when the
analysis service is down; the service's chain runner (copilot/dsp.py) is the
reference. This script synthesises a few deterministic signals, renders a set
of chains through the service's own code path (_run_serial_chain, then the
output stage _apply_output_gain at chain_output_ceiling_dbtp - exactly what
apply_pedalboard_chain does, without the file round trip) and records each
output's integrated loudness, true peak and octave-band levels, measured with
the service's meters. The C++ parity test (tests/shared-core/
test_offline_chain_renderer.cpp) rebuilds the signals from the recipes,
renders the same chains natively and compares (0.05 LU, 0.1 dB per band).

FN2-2a added the four processors that used to need the engine
(SpectralDenoise, TransientShaper, TapeSaturation, StereoWidener), a hissy
signal for the denoiser, and ``noise_floor``: the denoiser's reduction of
the hiss (the octave bands from 4 kHz up, where the signal is hiss only),
which the C++ test checks the native render reproduces within 0.1 dB.

Run from the repository root with the service venv:

    local-services/.venv/Scripts/python scripts/gen_render_parity_fixtures.py

Re-run it (and commit the JSON) when a chain processor changes on purpose;
local-services/tests/test_render_parity_fixture.py fails when the fixture no
longer matches the service.
"""
from __future__ import annotations

import importlib.util
import json
import sys
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "local-services"))

from khensu_services.analysis import loudness  # noqa: E402
from khensu_services.copilot import dsp  # noqa: E402

OUT = ROOT / "tests" / "shared-core" / "fixtures" / "render_parity.json"

# The signal recipes and the octave-band measurement are the meter-parity
# fixture's (same LCG, same Welch bands), so the C++ side shares its generator.
_spec = importlib.util.spec_from_file_location("gen_native_meter_fixtures",
                                               ROOT / "scripts" / "gen_native_meter_fixtures.py")
_meters = importlib.util.module_from_spec(_spec)
assert _spec.loader is not None
_spec.loader.exec_module(_meters)

SIGNALS = [
    {"name": "noise_tones_stereo_44k1", "sample_rate": 44100, "duration_sec": 4.0,
     "channels": [{"tones": [{"freq": 60.0, "amp": 0.2}, {"freq": 440.0, "amp": 0.1}],
                   "noise_seed": 11, "noise_amp": 0.15,
                   "gain_steps": [[0.0, 0.4], [1.0, 1.0], [2.5, 0.6]]},
                  {"tones": [{"freq": 60.0, "amp": 0.2}, {"freq": 3300.0, "amp": 0.05}],
                   "noise_seed": 12, "noise_amp": 0.15,
                   "gain_steps": [[0.0, 0.4], [1.0, 1.0], [2.5, 0.6]]}]},
    {"name": "noise_mono_48k", "sample_rate": 48000, "duration_sec": 3.0,
     "channels": [{"tones": [{"freq": 7000.0, "amp": 0.08}],
                   "noise_seed": 5, "noise_amp": 0.3,
                   "gain_steps": [[0.0, 0.2], [1.5, 1.0]]}]},
    # FN2-2a: tones over a steady hiss about 50 dB down (the noise-floor
    # finding's territory) - what the denoiser is for.
    {"name": "hiss_tones_stereo_44k1", "sample_rate": 44100, "duration_sec": 5.0,
     "channels": [{"tones": [{"freq": 220.0, "amp": 0.25}, {"freq": 1000.0, "amp": 0.08}],
                   "noise_seed": 21, "noise_amp": 0.004,
                   "gain_steps": [[0.0, 1.0], [2.0, 0.5], [3.5, 1.0]]},
                  {"tones": [{"freq": 330.0, "amp": 0.2}, {"freq": 1000.0, "amp": 0.08}],
                   "noise_seed": 22, "noise_amp": 0.004,
                   "gain_steps": [[0.0, 1.0], [2.0, 0.5], [3.5, 1.0]]}]},
]

CHAINS = [
    {"name": "highpass_80", "chain": [{"effect": "HighpassFilter", "parameters": {"cutoff_frequency_hz": 80.0}}]},
    {"name": "highpass_150_24db", "chain": [
        {"effect": "HighpassFilter", "parameters": {"cutoff_frequency_hz": 150.0, "slope_db_per_oct": 24.0}}]},
    {"name": "lowpass_5k", "chain": [{"effect": "LowpassFilter", "parameters": {"cutoff_frequency_hz": 5000.0}}]},
    {"name": "low_shelf_cut", "chain": [
        {"effect": "LowShelfFilter", "parameters": {"cutoff_frequency_hz": 120.0, "gain_db": -6.0, "q": 0.707}}]},
    {"name": "high_shelf_boost", "chain": [
        {"effect": "HighShelfFilter", "parameters": {"cutoff_frequency_hz": 4000.0, "gain_db": 4.0, "q": 0.707}}]},
    {"name": "peak_and_notch", "chain": [
        {"effect": "PeakFilter", "parameters": {"cutoff_frequency_hz": 1000.0, "gain_db": 6.0, "q": 1.0}},
        {"effect": "PeakFilter", "parameters": {"cutoff_frequency_hz": 3150.0, "gain_db": -9.0, "q": 6.0}}]},
    {"name": "compressor", "chain": [
        {"effect": "Compressor", "parameters": {"threshold_db": -24.0, "ratio": 4.0, "attack_ms": 5.0,
                                                "release_ms": 100.0}}]},
    {"name": "deesser", "chain": [{"effect": "DeEsser", "parameters": {"threshold_db": -36.0,
                                                                        "frequency_hz": 7000.0}}]},
    {"name": "gain_into_clip_guard", "chain": [{"effect": "Gain", "parameters": {"gain_db": 8.0}}]},
    {"name": "gain_into_limiter", "chain": [
        {"effect": "Gain", "parameters": {"gain_db": 9.0}},
        {"effect": "Limiter", "parameters": {"threshold_db": -1.0}}]},
    {"name": "fix_chain", "chain": [
        {"effect": "HighpassFilter", "parameters": {"cutoff_frequency_hz": 30.0}},
        {"effect": "LowShelfFilter", "parameters": {"cutoff_frequency_hz": 150.0, "gain_db": -4.0, "q": 0.707}},
        {"effect": "PeakFilter", "parameters": {"cutoff_frequency_hz": 2500.0, "gain_db": -3.0, "q": 2.0}},
        {"effect": "Compressor", "parameters": {"threshold_db": -20.0, "ratio": 2.0, "attack_ms": 10.0,
                                                "release_ms": 150.0}},
        {"effect": "Gain", "parameters": {"gain_db": 4.0}},
        {"effect": "Limiter", "parameters": {"threshold_db": -1.5}}]},
    # FN2-2a: the processors that used to need the engine.
    {"name": "spectral_denoise", "chain": [{"effect": "SpectralDenoise", "parameters": {"reduction_db": 12.0}}]},
    {"name": "spectral_denoise_gentle", "chain": [
        {"effect": "SpectralDenoise", "parameters": {"reduction_db": 6.0, "sensitivity": 2.0}}]},
    {"name": "transient_shaper", "chain": [
        {"effect": "TransientShaper", "parameters": {"attack_gain_db": 6.0, "sustain_gain_db": -3.0,
                                                     "attack_ms": 5.0, "release_ms": 150.0}}]},
    {"name": "tape_saturation", "chain": [
        {"effect": "TapeSaturation", "parameters": {"drive_db": 9.0, "warmth": 0.7, "mix": 0.8,
                                                    "output_trim_db": -2.0}}]},
    {"name": "stereo_widener", "chain": [{"effect": "StereoWidener", "parameters": {"width": 1.5}}]},
    # A stage in front of the denoiser: its profile pass runs through it.
    {"name": "restoration_chain", "chain": [
        {"effect": "HighpassFilter", "parameters": {"cutoff_frequency_hz": 30.0}},
        {"effect": "SpectralDenoise", "parameters": {"reduction_db": 12.0}},
        {"effect": "TransientShaper", "parameters": {"attack_gain_db": 3.0}},
        {"effect": "TapeSaturation", "parameters": {"drive_db": 6.0}},
        {"effect": "StereoWidener", "parameters": {"width": 0.8}},
        {"effect": "Gain", "parameters": {"gain_db": 2.0}},
        {"effect": "Limiter", "parameters": {"threshold_db": -1.0}}]},
]

# The denoiser's job, measured: the hiss bands (no tone above 4 kHz).
NOISE_FLOOR = {"signal": "hiss_tones_stereo_44k1", "chain_name": "spectral_denoise", "from_hz": 4000.0}


def _finite(v):
    return None if v is None or not np.isfinite(v) else float(v)


def render(x: np.ndarray, sr: int, chain: list[dict]) -> np.ndarray:
    """The service's render of ``chain`` (apply_pedalboard_chain minus file IO)."""
    audio = x.astype(np.float64) if x.shape[0] > 1 else x[0].astype(np.float64)
    processed, _applied, skipped, _h, _s = dsp._run_serial_chain(audio, sr, chain)
    assert not skipped, skipped
    processed = dsp._apply_output_gain(processed, False, dsp.chain_output_ceiling_dbtp(chain))
    return np.atleast_2d(processed)


def measure(y: np.ndarray, sr: int) -> dict:
    centers, levels = _meters.octave_levels(y.mean(axis=0), sr)
    return {
        "integrated_lufs": _finite(loudness.integrated_lufs(y, sr)),
        "true_peak_dbtp": float(loudness.true_peak_dbtp(y, sr)),
        "octave_centers_hz": centers,
        "octave_levels_db": levels,
    }


def floor_db(y: np.ndarray, sr: int, from_hz: float) -> float:
    """Mean octave-band level from ``from_hz`` up (the hiss-only bands)."""
    centers, levels = _meters.octave_levels(y.mean(axis=0), sr)
    return float(np.mean([lv for fc, lv in zip(centers, levels) if fc >= from_hz]))


def build() -> dict:
    cases = []
    noise_floor = dict(NOISE_FLOOR)
    for spec in SIGNALS:
        x = _meters.synthesize(spec)
        sr = int(spec["sample_rate"])
        for chain in CHAINS:
            y = render(x, sr, chain["chain"])
            cases.append({"signal": spec["name"], "chain_name": chain["name"], "chain": chain["chain"],
                          "expected": measure(y, sr)})
            if spec["name"] == NOISE_FLOOR["signal"] and chain["name"] == NOISE_FLOOR["chain_name"]:
                noise_floor["input_floor_db"] = floor_db(x.astype(np.float64), sr, NOISE_FLOOR["from_hz"])
                noise_floor["output_floor_db"] = floor_db(y, sr, NOISE_FLOOR["from_hz"])
    return {
        "generator": "scripts/gen_render_parity_fixtures.py",
        "loudness_tolerance": 0.05,
        "band_tolerance_db": 0.1,
        "true_peak_tolerance_db": 0.1,
        "noise_floor_tolerance_db": 0.1,
        "signals": SIGNALS,
        "cases": cases,
        "noise_floor": noise_floor,
    }


def main() -> int:
    fixtures = build()
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(json.dumps(fixtures, indent=2) + "\n", encoding="utf-8")
    print(f"wrote {OUT.relative_to(ROOT)} ({len(fixtures['cases'])} cases)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
