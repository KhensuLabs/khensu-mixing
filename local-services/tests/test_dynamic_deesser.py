"""Tests for the DynamicDeEsser (program-dependent de-esser — the detector is
the sibilance band's level RELATIVE to the whole signal, so quiet and loud
passages are de-essed by the same amount; the static DeEsser's absolute
threshold misses quiet esses and over-triggers on loud ones)."""
from __future__ import annotations

import numpy as np
import pytest

from khensu_services.copilot.dsp import _apply_dynamic_deesser, _run_serial_chain

SR = 44100


def _band_rms(y: np.ndarray, sr: int, lo: float, hi: float) -> float:
    spec = np.fft.rfft(y)
    freqs = np.fft.rfftfreq(y.shape[0], 1.0 / sr)
    mask = (freqs >= lo) & (freqs <= hi)
    banded = np.fft.irfft(spec * mask, n=y.shape[0])
    return float(np.sqrt(np.mean(banded ** 2)))


@pytest.fixture
def sibilant_vocal():
    """1 s loud + 1 s quiet 'vocal': 300 Hz body with a hot 7 kHz 'ess' mixed
    at the same RELATIVE level in both halves. The quiet half sits 26 dB
    down — far below any sane absolute threshold."""
    t = np.arange(SR, dtype=np.float64) / SR
    seg = 0.5 * np.sin(2 * np.pi * 300 * t) + 0.5 * np.sin(2 * np.pi * 7000 * t)
    mono = np.concatenate([seg, 0.05 * seg])
    return mono, np.stack([mono, mono])


def test_dynamic_deesser_preserves_shape_and_stays_finite(sibilant_vocal):
    mono, stereo = sibilant_vocal
    out_m = _apply_dynamic_deesser(mono, SR)
    out_s = _apply_dynamic_deesser(stereo, SR)
    assert out_m.shape == mono.shape and out_m.ndim == 1
    assert out_s.shape == stereo.shape
    assert np.isfinite(out_m).all() and np.isfinite(out_s).all()


def test_dynamic_deesser_tracks_program_level(sibilant_vocal):
    mono, _ = sibilant_vocal
    out = _apply_dynamic_deesser(mono, SR, frequency_hz=7000.0,
                                 rel_threshold_db=-12.0, ratio=8.0)
    edge = SR // 10
    loud_in, loud_out = mono[edge:SR - edge], out[edge:SR - edge]
    quiet_in, quiet_out = mono[SR + edge:-edge], out[SR + edge:-edge]

    def reduction_db(before, after):
        return 20.0 * np.log10(_band_rms(after, SR, 6000, 8000)
                               / _band_rms(before, SR, 6000, 8000))

    loud_red = reduction_db(loud_in, loud_out)
    quiet_red = reduction_db(quiet_in, quiet_out)
    # Both halves get real de-essing…
    assert loud_red < -3.0
    assert quiet_red < -3.0
    # …and by the SAME amount: the detector is relative, not absolute.
    assert loud_red == pytest.approx(quiet_red, abs=1.5)


def test_dynamic_deesser_quiet_ess_beats_static_threshold(sibilant_vocal):
    from khensu_services.copilot.dsp import _apply_deesser
    mono, _ = sibilant_vocal
    quiet = mono[SR:]
    # A static threshold placed for the loud half does nothing to the -26 dB
    # half; the dynamic detector still catches it.
    static = _apply_deesser(quiet, SR, threshold_db=-12.0, ratio=8.0,
                            frequency_hz=7000.0)
    dynamic = _apply_dynamic_deesser(quiet, SR, frequency_hz=7000.0,
                                     rel_threshold_db=-12.0, ratio=8.0)
    edge = SR // 10
    assert _band_rms(static[edge:-edge], SR, 6000, 8000) == pytest.approx(
        _band_rms(quiet[edge:-edge], SR, 6000, 8000), rel=0.10)
    assert (_band_rms(dynamic[edge:-edge], SR, 6000, 8000)
            < 0.7 * _band_rms(quiet[edge:-edge], SR, 6000, 8000))


def test_dynamic_deesser_leaves_non_sibilant_audio_alone():
    t = np.arange(SR) / SR
    body = 0.4 * np.sin(2 * np.pi * 300 * t)   # no HF content at all
    out = _apply_dynamic_deesser(body, SR, frequency_hz=7000.0)
    np.testing.assert_allclose(out, body, atol=1e-6)


def test_dynamic_deesser_respects_max_reduction_floor(sibilant_vocal):
    mono, _ = sibilant_vocal
    out = _apply_dynamic_deesser(mono, SR, frequency_hz=7000.0,
                                 rel_threshold_db=-30.0, ratio=20.0,
                                 max_reduction_db=3.0)
    edge = SR // 10
    red_db = 20.0 * np.log10(
        _band_rms(out[edge:SR - edge], SR, 6000, 8000)
        / _band_rms(mono[edge:SR - edge], SR, 6000, 8000))
    # Extreme settings, but the cut may never exceed the 3 dB cap
    # (small tolerance for envelope edges).
    assert red_db >= -3.5


def test_dynamic_deesser_degenerate_band_is_noop():
    y = np.sin(2 * np.pi * 440 * np.arange(SR // 4) / SR)
    # At 6 kHz sample rate the sibilance band collapses → pass through.
    out = _apply_dynamic_deesser(y, 6000, frequency_hz=9000.0)
    np.testing.assert_array_equal(out, y)


def _max_reduction_step_db(npts: int) -> float:
    """Sweep input level through the presence-gate window and return the largest
    gain-reduction change between neighbouring levels."""
    from khensu_services.copilot.dsp import _DEESS_GATE_HI, _DEESS_GATE_LO

    t = np.arange(SR // 8, dtype=np.float64) / SR
    ess = 0.5 * np.sin(2 * np.pi * 300 * t) + 0.5 * np.sin(2 * np.pi * 7000 * t)
    unit = ess / max(np.mean(np.abs(ess)), 1e-12)

    reductions = []
    for lvl in np.geomspace(_DEESS_GATE_LO * 0.2, _DEESS_GATE_HI * 5.0, npts):
        scaled = unit * lvl
        out = _apply_dynamic_deesser(scaled, SR, frequency_hz=7000.0,
                                     rel_threshold_db=-12.0, ratio=8.0)
        edge = len(scaled) // 10
        before = _band_rms(scaled[edge:-edge], SR, 6000, 8000)
        after = _band_rms(out[edge:-edge], SR, 6000, 8000)
        reductions.append(20.0 * np.log10(max(after, 1e-30) / max(before, 1e-30)))

    reductions = np.asarray(reductions)
    assert np.isfinite(reductions).all()
    return float(np.abs(np.diff(reductions)).max())


def test_dynamic_deesser_gate_is_continuous_across_the_silence_boundary():
    """AUD-04: the presence gate must ramp, not switch.

    A sibilant tail decaying through ~-80 dBFS used to flip the detector
    between 0 and its full value sample-to-sample, modulating band gain on
    exactly the quiet decays where it is most audible.

    Asserted as *convergence* rather than a fixed dB bound, because that is
    what actually distinguishes a continuous gate from a step: refining the
    level sweep must shrink the largest jump proportionally. Measured, the old
    hard `full_env > 1e-4` gate gave 5.31 dB at 120 points and 5.52 dB at 240
    -- it does not shrink, because the cliff is real at every resolution. The
    ramp gives 0.64 -> 0.32 dB, halving with the step size.
    """
    coarse = _max_reduction_step_db(100)
    fine = _max_reduction_step_db(200)

    assert fine < 1.0, f"gate jump too large to be continuous: {fine:.2f} dB"
    assert fine < 0.75 * coarse, (
        f"jump does not shrink with sweep resolution ({coarse:.2f} -> {fine:.2f} dB); "
        "that is a discontinuity, not a steep ramp"
    )


def test_dynamic_deesser_gate_end_states_are_unchanged():
    """The ramp must not alter behaviour outside the window: fully muted below
    -80 dBFS, fully live well above -70 dBFS."""
    from khensu_services.copilot.dsp import _DEESS_GATE_LO

    t = np.arange(SR // 4, dtype=np.float64) / SR
    ess = 0.5 * np.sin(2 * np.pi * 300 * t) + 0.5 * np.sin(2 * np.pi * 7000 * t)
    unit = ess / max(np.mean(np.abs(ess)), 1e-12)

    silent = unit * (_DEESS_GATE_LO * 0.5)
    out_silent = _apply_dynamic_deesser(silent, SR, frequency_hz=7000.0)
    np.testing.assert_allclose(out_silent, silent, atol=1e-12)

    loud = unit * 0.2
    out_loud = _apply_dynamic_deesser(loud, SR, frequency_hz=7000.0,
                                      rel_threshold_db=-12.0, ratio=8.0)
    edge = len(loud) // 10
    red = 20.0 * np.log10(_band_rms(out_loud[edge:-edge], SR, 6000, 8000)
                          / _band_rms(loud[edge:-edge], SR, 6000, 8000))
    assert red < -3.0


def test_chain_dispatches_dynamic_deesser(sibilant_vocal):
    _, stereo = sibilant_vocal
    processed, applied, skipped, _hr, _sl = _run_serial_chain(stereo, SR, [
        {"effect": "DynamicDeEsser", "parameters": {
            # Out-of-range values must clamp, not crash.
            "frequency_hz": 99999.0, "rel_threshold_db": -99.0,
            "ratio": 100.0, "max_reduction_db": 99.0}},
    ])
    assert applied == ["DynamicDeEsser"]
    assert skipped == []
    assert processed.shape == stereo.shape
    assert np.isfinite(processed).all()
