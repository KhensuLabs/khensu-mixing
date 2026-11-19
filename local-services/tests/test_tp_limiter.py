"""True-peak lookahead limiter (AUDIT-006): the ceiling every render path uses
in place of pedalboard.Limiter (a maximiser with ~+(3.75 - th) dB makeup)."""
from __future__ import annotations

import numpy as np
import pytest

from khensu_services.analysis.loudness import true_peak_dbtp
from khensu_services.copilot.tp_limiter import true_peak_limit

SR = 44100


def _sine(seconds: float = 2.0, amp: float = 1.0, freq: float = 997.0) -> np.ndarray:
    t = np.arange(int(SR * seconds)) / SR
    s = amp * np.sin(2 * np.pi * freq * t)
    return np.stack([s, 0.8 * s])


def _drum_loop(seconds: float = 4.0) -> np.ndarray:
    """Kick-like decaying bursts on the beat with quieter off-beat hats."""
    rng = np.random.default_rng(7)
    n = int(SR * seconds)
    y = np.zeros(n)
    step = SR // 4
    for k in range(0, n, step):
        m = min(6000, n - k)
        env = np.exp(-np.arange(m) / 500.0)
        if (k // step) % 2 == 0:
            tt = np.arange(m) / SR
            y[k:k + m] += np.sin(2 * np.pi * 60.0 * tt) * env
            y[k:k + m] += 0.3 * rng.standard_normal(m) * env
        else:
            y[k:k + m] += 0.4 * rng.standard_normal(m) * np.exp(-np.arange(m) / 120.0)
    y /= np.max(np.abs(y))
    return np.stack([y, np.roll(y, 5)])


@pytest.mark.parametrize("ceiling", [-1.0, -0.3, -6.0])
def test_sine_driven_12db_over_lands_on_the_ceiling(ceiling):
    x = _sine(amp=10.0 ** ((ceiling + 12.0) / 20.0))
    y = true_peak_limit(x, SR, ceiling_dbtp=ceiling)
    assert true_peak_dbtp(y) <= ceiling + 0.1
    # Steady-state: a sine held over the ceiling sits right at it (not 4-8 dB
    # under, which is what a clip-then-rescale gives).
    core = y[:, SR // 2:-SR // 2]
    assert 20 * np.log10(np.max(np.abs(core))) == pytest.approx(ceiling, abs=0.3)


def test_drum_loop_driven_12db_over_holds_the_true_peak_ceiling():
    x = _drum_loop() * 10.0 ** (12.0 / 20.0)
    y = true_peak_limit(x, SR, ceiling_dbtp=-1.0)
    assert true_peak_dbtp(x) > 10.0
    assert true_peak_dbtp(y) <= -1.0 + 0.1
    assert y.shape == x.shape and y.dtype == x.dtype
    assert np.isfinite(y).all()


def test_no_output_shift():
    """The lookahead lives in the gain curve, so the output is sample-aligned
    with the input: the limited signal is the input times a positive gain."""
    x = _drum_loop() * 4.0
    y = true_peak_limit(x, SR, ceiling_dbtp=-1.0)
    nz = np.abs(x) > 1e-6
    ratio = y[nz] / x[nz]
    assert np.all(ratio > 0.0) and np.all(ratio <= 1.0 + 1e-12)
    # Same gain on both channels (linked) wherever both are non-silent.
    both = nz[0] & nz[1]
    np.testing.assert_allclose(y[0, both] / x[0, both], y[1, both] / x[1, both], rtol=1e-9)
    # Cross-correlation peaks at lag 0.
    xc = np.correlate(y[0, :20000], x[0, :20000], mode="full")
    assert int(np.argmax(xc)) - (20000 - 1) == 0


def test_unity_below_the_ceiling():
    x = _drum_loop() * 0.5           # ~-6 dBTP, well under -1
    y = true_peak_limit(x, SR, ceiling_dbtp=-1.0)
    np.testing.assert_array_equal(y, x)


def test_adds_no_makeup_gain_to_quiet_material():
    # pedalboard.Limiter(-1) lifted a -20 dBFS sine to -15.25 dBFS.
    x = _sine(amp=10.0 ** (-20.0 / 20.0))
    y = true_peak_limit(x, SR, ceiling_dbtp=-1.0)
    np.testing.assert_array_equal(y, x)


def test_mono_1d_and_float32_round_trip():
    x = (_sine(amp=2.0)[0]).astype(np.float32)
    y = true_peak_limit(x, SR, ceiling_dbtp=-1.0)
    assert y.shape == x.shape and y.dtype == np.float32
    assert true_peak_dbtp(y.astype(np.float64)) <= -1.0 + 0.1


def test_chain_limiter_is_the_true_peak_ceiling():
    """The "Limiter" EffectSpec the LLM/presets use now means a dBTP ceiling."""
    from khensu_services.copilot.dsp import _run_serial_chain

    x = _drum_loop() * 4.0
    y, applied, skipped, _hr, _lv = _run_serial_chain(
        x, SR, [{"effect": "Limiter", "parameters": {"threshold_db": -2.0}}])
    assert applied == ["Limiter"] and skipped == []
    assert true_peak_dbtp(y) <= -2.0 + 0.1

    quiet = _sine(amp=0.1)
    yq, *_ = _run_serial_chain(quiet, SR, [{"effect": "Limiter", "parameters": {"threshold_db": -1.0}}])
    np.testing.assert_allclose(yq, quiet, atol=1e-12)


def test_gain_plus_limiter_preset_raises_level_without_overs():
    from khensu_services.copilot.dsp import _run_serial_chain

    x = _drum_loop() * 0.7
    y, *_ = _run_serial_chain(x, SR, [
        {"effect": "Gain", "parameters": {"gain_db": 3.0}},
        {"effect": "Limiter", "parameters": {"threshold_db": -1.0}},
    ])
    assert true_peak_dbtp(y) <= -1.0 + 0.1
    assert np.sqrt(np.mean(y ** 2)) > np.sqrt(np.mean(x ** 2))
