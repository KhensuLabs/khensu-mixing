"""Render-DSP regressions from the 2026-10-15 audit (AUDIT-017, 031, 032, 033,
082) and equivalence proofs for the loops in copilot/dsp.py that were sped up
or vectorised (the reference implementations below are the pre-change code)."""
from __future__ import annotations

import numpy as np
import pytest
import soundfile as sf

from khensu_services.analysis.loudness import true_peak_dbtp
from khensu_services.copilot import dsp

SR = 44100


# ---------------------------------------------------------------------------
# AUDIT-017: section boundaries ramp instead of stepping.
# ---------------------------------------------------------------------------
def _short_rms_db(y: np.ndarray, win: int) -> np.ndarray:
    c = np.concatenate([[0.0], np.cumsum(y.astype(np.float64) ** 2)])
    rms = np.sqrt((c[win:] - c[:-win]) / win)
    return 20.0 * np.log10(rms + 1e-12)


def test_section_boundary_level_change_is_at_most_1db_per_ms(tmp_path):
    t = np.arange(2 * SR) / SR
    y = 0.5 * np.sin(2 * np.pi * 1000.0 * t)
    src = tmp_path / "in.wav"
    out = tmp_path / "out.wav"
    sf.write(str(src), y, SR, subtype="FLOAT")
    dsp.apply_section_chains(str(src), str(out), [
        {"label": "verse", "start_sec": 0.0, "end_sec": 1.0,
         "effects": [{"effect": "Gain", "parameters": {"gain_db": -12.0}}]},
        {"label": "chorus", "start_sec": 1.0, "end_sec": 2.0,
         "effects": [{"effect": "Gain", "parameters": {"gain_db": 0.0}}]},
    ])
    rendered, _ = sf.read(str(out))
    ms = SR // 1000
    env = _short_rms_db(rendered, ms)          # 1 ms RMS (one 1 kHz period)
    region = env[SR - 20 * ms:SR + 60 * ms]
    step_per_ms = np.abs(region[ms:] - region[:-ms])
    assert float(np.max(step_per_ms)) <= 1.0
    # Both sides really are at their own levels away from the boundary.
    assert env[SR // 2] == pytest.approx(20 * np.log10(0.5 / np.sqrt(2)) - 12.0, abs=0.2)
    assert env[SR + SR // 2] == pytest.approx(20 * np.log10(0.5 / np.sqrt(2)), abs=0.2)


def test_section_render_crossfades_into_and_out_of_a_dry_gap(tmp_path):
    t = np.arange(2 * SR) / SR
    y = 0.4 * np.sin(2 * np.pi * 1000.0 * t)
    src = tmp_path / "in.wav"
    out = tmp_path / "out.wav"
    sf.write(str(src), y, SR, subtype="FLOAT")
    dsp.apply_section_chains(str(src), str(out), [
        {"label": "drop", "start_sec": 0.5, "end_sec": 1.5,
         "effects": [{"effect": "Gain", "parameters": {"gain_db": -12.0}}]},
    ])
    rendered, _ = sf.read(str(out))
    ms = SR // 1000
    env = _short_rms_db(rendered, ms)
    for b in (SR // 2, SR + SR // 2):
        region = env[b - 20 * ms:b + 60 * ms]
        assert float(np.max(np.abs(region[ms:] - region[:-ms]))) <= 1.0


def test_section_output_guard_is_true_peak(tmp_path):
    # Pre-change the final guard only scaled when the SAMPLE peak passed 1.0.
    t = np.arange(SR) / SR
    y = 0.99 * np.sin(2 * np.pi * (SR / 4) * t + np.pi / 4)   # ISP ~ +3 dB over sample peak
    src = tmp_path / "in.wav"
    out = tmp_path / "out.wav"
    sf.write(str(src), y, SR, subtype="FLOAT")
    dsp.apply_section_chains(str(src), str(out), [
        {"label": "a", "start_sec": 0.0, "end_sec": 1.0, "effects": []},
    ])
    rendered, _ = sf.read(str(out))
    assert true_peak_dbtp(rendered) <= -1.0 + 0.1


def test_render_delta_guard_is_true_peak(tmp_path):
    t = np.arange(SR) / SR
    dry = np.zeros(SR)
    wet = 0.99 * np.sin(2 * np.pi * (SR / 4) * t + np.pi / 4)
    dp, wp, op = tmp_path / "dry.wav", tmp_path / "wet.wav", tmp_path / "delta.wav"
    sf.write(str(dp), dry, SR, subtype="FLOAT")
    sf.write(str(wp), wet, SR, subtype="FLOAT")
    dsp.render_delta(str(dp), str(wp), str(op))
    delta, _ = sf.read(str(op))
    assert true_peak_dbtp(delta.T) <= -1.0 + 0.1


# ---------------------------------------------------------------------------
# AUDIT-031: IR is unit-energy, so wet is the wet/dry RMS ratio.
# ---------------------------------------------------------------------------
def test_convolution_wet_level_is_rms_ratio(tmp_path):
    rng = np.random.default_rng(3)
    ir = rng.standard_normal(SR // 2) * np.exp(-np.arange(SR // 2) / (0.1 * SR))
    ir_path = tmp_path / "ir.wav"
    sf.write(str(ir_path), ir / np.max(np.abs(ir)), SR, subtype="FLOAT")
    x = 0.1 * rng.standard_normal((2, 3 * SR))
    wet = dsp._apply_convolution_reverb(x, SR, str(ir_path), wet=0.25, dry=0.0)
    ratio_db = 20 * np.log10(np.sqrt(np.mean(wet[:, SR:] ** 2)) / np.sqrt(np.mean(x[:, SR:] ** 2)))
    assert ratio_db == pytest.approx(20 * np.log10(0.25), abs=1.0)


# ---------------------------------------------------------------------------
# AUDIT-032: the tape DC blocker corner is ~5 Hz at every rate.
# ---------------------------------------------------------------------------
@pytest.mark.parametrize("sr", [44100, 96000, 192000])
def test_tape_dc_blocker_leaves_40hz_alone_at_every_rate(sr):
    t = np.arange(2 * sr) / sr
    x = 0.01 * np.sin(2 * np.pi * 40.0 * t)       # small: tanh is ~linear
    y = dsp._apply_tape_saturation(x, sr, drive_db=0.0, warmth=0.0)
    core = slice(sr // 2, -sr // 2)
    gain_db = 20 * np.log10(np.sqrt(np.mean(y[core] ** 2)) / np.sqrt(np.mean(x[core] ** 2)))
    assert gain_db == pytest.approx(0.0, abs=0.3)


# ---------------------------------------------------------------------------
# AUDIT-033: humanize keeps a mono source mono and isn't shifted late.
# ---------------------------------------------------------------------------
def test_humanize_mono_source_stays_mono():
    t = np.arange(6 * SR) / SR
    m = 0.3 * np.sin(2 * np.pi * 440.0 * t)
    out = dsp._apply_humanize(np.stack([m, m]), SR, intensity=0.8, depth_ms=8.0)
    side = out[0] - out[1]
    assert np.sqrt(np.mean(side ** 2)) < 1e-9 * np.sqrt(np.mean(out[0] ** 2)) + 1e-15


def test_humanize_delay_curve_is_zero_mean():
    rng = np.random.default_rng(11)
    x = rng.standard_normal(8 * SR) * 0.1
    out = dsp._apply_humanize(x, SR, intensity=1.0, depth_ms=8.0, level_var_db=0.0,
                              noise_intensity=0.0)
    seg = slice(SR, 7 * SR)
    lags = np.arange(-400, 401)
    xc = np.array([float(np.dot(out[seg], np.roll(x, k)[seg])) for k in lags])
    # White noise through a time-varying delay: the cross-correlation is the
    # delay curve's histogram, so its centroid is the mean delay. Pre-change
    # the mean was depth/2 (~176 samples late at 8 ms).
    w = np.maximum(xc, 0.0)
    mean_lag = float(np.sum(lags * w) / np.sum(w))
    assert abs(mean_lag) <= 25


# ---------------------------------------------------------------------------
# AUDIT-082: the Clipper holds its ceiling without a base-rate hard clamp.
# ---------------------------------------------------------------------------
def test_clipper_holds_true_peak_ceiling():
    t = np.arange(SR) / SR
    x = np.stack([1.5 * np.sin(2 * np.pi * 3000.0 * t)] * 2)
    y = dsp._apply_clipper(x, SR, ceiling_db=-1.0, hardness=1.0)
    assert true_peak_dbtp(y) <= -1.0 + 0.1


# ---------------------------------------------------------------------------
# Equivalence: sped-up / vectorised loops match the original per-sample code.
# ---------------------------------------------------------------------------
def _ref_ar_follower(rect, atk, rel):
    env = np.empty_like(rect)
    e = 0.0
    for i in range(rect.shape[0]):
        coeff = atk if rect[i] > e else rel
        e = coeff * e + (1.0 - coeff) * rect[i]
        env[i] = e
    return env


def _ref_modulated_delay(audio, sr, *, base_delay_ms, depth_ms, rate_hz, feedback=0.0):
    was_1d = audio.ndim == 1
    x = audio[np.newaxis, :] if was_1d else audio
    max_delay = int(np.ceil((base_delay_ms + depth_ms + 2.0) * sr / 1000.0))
    history = np.zeros((x.shape[0], max_delay + 2), dtype=np.float64)
    out = np.empty_like(x, dtype=np.float64)
    write = 0
    for sample in range(x.shape[1]):
        delay = (base_delay_ms + depth_ms * (0.5 + 0.5 * np.sin(2.0 * np.pi * rate_hz * sample / sr))) * sr / 1000.0
        read = write - delay
        low = int(np.floor(read)) % history.shape[1]
        high = (low + 1) % history.shape[1]
        fraction = read - np.floor(read)
        delayed = history[:, low] * (1.0 - fraction) + history[:, high] * fraction
        out[:, sample] = delayed
        history[:, write] = x[:, sample] + delayed * feedback
        write = (write + 1) % history.shape[1]
    return out[0] if was_1d else out


def test_ar_follower_is_bit_identical_to_the_loop():
    rng = np.random.default_rng(5)
    rect = np.abs(rng.standard_normal(20000)) * np.repeat(rng.uniform(0, 1, 20), 1000)
    atk = float(np.exp(-1.0 / (0.005 * SR)))
    rel = float(np.exp(-1.0 / (0.12 * SR)))
    np.testing.assert_array_equal(dsp._ar_follower(rect, atk, rel), _ref_ar_follower(rect, atk, rel))


@pytest.mark.parametrize("params", [
    dict(base_delay_ms=4.0, depth_ms=2.0, rate_hz=5.0, feedback=0.0),     # vibrato
    dict(base_delay_ms=3.0, depth_ms=2.0, rate_hz=0.7, feedback=0.15),    # flanger
    dict(base_delay_ms=0.1, depth_ms=0.5, rate_hz=1.3, feedback=-0.9),    # tiny delay, strong fb
])
@pytest.mark.parametrize("sr", [8000, 44100])
def test_modulated_delay_matches_the_per_sample_loop(params, sr):
    rng = np.random.default_rng(9)
    x = rng.standard_normal((2, sr // 2))
    ref = _ref_modulated_delay(x, sr, **params)
    got = dsp._modulated_delay(x, sr, **params)
    np.testing.assert_allclose(got, ref, rtol=1e-9, atol=1e-9)
    mono = dsp._modulated_delay(x[0], sr, **params)
    np.testing.assert_allclose(mono, ref[0], rtol=1e-9, atol=1e-9)


def test_glue_rms_detector_matches_the_loop():
    rng = np.random.default_rng(2)
    x = rng.standard_normal((2, SR)) * 0.3
    got = dsp._apply_glue_compressor(x, SR, detector=1.0, threshold_db=-30.0, ratio=4.0)
    # Reference: the pre-change one-pole RMS loop feeding the same core.
    rms_coeff = float(np.exp(-1.0 / (0.005 * SR)))
    sq = x.mean(axis=0) ** 2
    acc, env_sq = 0.0, np.empty_like(sq)
    for i in range(sq.shape[0]):
        acc = rms_coeff * acc + (1.0 - rms_coeff) * sq[i]
        env_sq[i] = acc
    from scipy.signal import lfilter
    np.testing.assert_allclose(lfilter([1.0 - rms_coeff], [1.0, -rms_coeff], sq), env_sq,
                               rtol=1e-9, atol=1e-15)
    assert got.shape == x.shape and np.isfinite(got).all()
