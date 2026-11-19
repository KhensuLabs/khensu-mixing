# local-services/tests/test_humanize.py
"""Tests for the Humanize / Analog Feel DSP effect (spec §5.2): wow/flutter
micro-timing, level micro-automation and a breathing noise floor - the analog
character of a played, taped and hand-ridden performance. The delivery-metadata
cleaner is tested in test_metadata_clean.py."""
import numpy as np

from khensu_services.copilot.dsp import _apply_humanize, _run_serial_chain

SR = 44100


def _tone(dur=6.0, freq=440.0, amp=0.3):
    t = np.arange(int(SR * dur)) / SR
    return (amp * np.sin(2 * np.pi * freq * t)).astype(np.float32)


def test_humanize_zero_intensity_is_exact_passthrough():
    y = _tone()
    out = _apply_humanize(y, SR, intensity=0.0)
    np.testing.assert_array_equal(out, y)


def test_humanize_preserves_shape_and_stays_finite():
    mono = _tone()
    stereo = np.stack([mono, mono])
    out_m = _apply_humanize(mono, SR, intensity=0.5)
    out_s = _apply_humanize(stereo, SR, intensity=0.5)
    assert out_m.shape == mono.shape
    assert out_s.shape == stereo.shape
    assert np.isfinite(out_m).all() and np.isfinite(out_s).all()
    # No runaway gain: level ride is capped at level_var_db.
    assert np.max(np.abs(out_m)) < np.max(np.abs(mono)) * 1.6


def test_humanize_is_deterministic_per_seed():
    y = _tone()
    a = _apply_humanize(y, SR, intensity=0.5, seed=42.0)
    b = _apply_humanize(y, SR, intensity=0.5, seed=42.0)
    c = _apply_humanize(y, SR, intensity=0.5, seed=43.0)
    np.testing.assert_array_equal(a, b)
    assert not np.array_equal(a, c)


def test_humanize_widens_level_distribution():
    # The signal AI detectors flag is *uniform* level; humanize must widen
    # the frame-level spread on a perfectly steady tone.
    y = _tone(dur=12.0)
    out = _apply_humanize(y, SR, intensity=0.8, level_var_db=1.5)

    def frame_db_std(sig):
        win = SR  # 1s frames
        n = len(sig) // win
        rms = np.sqrt(np.mean(sig[: n * win].reshape(n, win) ** 2, axis=1))
        return float(np.std(20.0 * np.log10(rms + 1e-9)))

    assert frame_db_std(out) > frame_db_std(y) + 0.2


def test_humanize_introduces_micro_pitch_variation():
    # Varying delay = varying pitch: the instantaneous frequency of a pure
    # tone should wander after humanize (tape wow), staying subtle.
    import librosa
    y = _tone(dur=8.0)
    out = _apply_humanize(y, SR, intensity=0.8, depth_ms=8.0, wow_rate_hz=0.5)
    f0 = librosa.yin(out.astype(np.float64), fmin=300, fmax=600,
                     sr=SR, frame_length=4096)
    f0 = f0[np.isfinite(f0)]
    spread = float(np.percentile(f0, 95) - np.percentile(f0, 5))
    assert 0.3 < spread < 15.0, f"pitch wander {spread:.2f} Hz out of range"


def test_humanize_raises_noise_floor_softly():
    y = _tone(dur=6.0, amp=0.4)
    out = _apply_humanize(y, SR, intensity=0.8, noise_floor_db=-60.0)
    # Quiet 100 ms at the start (sine crosses zero); compare floors via the
    # residual after removing the tone: check spectral floor instead —
    # total RMS barely changes but broadband energy appears.
    spec_in = np.abs(np.fft.rfft(y))
    spec_out = np.abs(np.fft.rfft(out))
    freqs = np.fft.rfftfreq(len(y), 1.0 / SR)
    band = (freqs > 3000) & (freqs < 8000)   # far from the 440 Hz tone
    assert spec_out[band].mean() > spec_in[band].mean() * 3.0


def test_chain_dispatches_humanize():
    stereo = np.stack([_tone(), _tone()])
    processed, applied, skipped, _hr, _sl = _run_serial_chain(stereo, SR, [
        {"effect": "Humanize", "parameters": {
            # Out-of-range values must clamp, not crash.
            "intensity": 5.0, "depth_ms": 99.0, "wow_rate_hz": 0.0,
            "level_var_db": 99.0, "noise_floor_db": 0.0}},
    ])
    assert applied == ["Humanize"]
    assert skipped == []
    assert processed.shape == stereo.shape
    assert np.isfinite(processed).all()
