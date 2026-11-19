"""Tests for pitch-independent spectral-envelope formant shifting."""
from __future__ import annotations

import numpy as np

from khensu_services.copilot.dsp import _apply_formant_shifter, _run_serial_chain


SR = 44_100


def _vocalish_harmonics() -> np.ndarray:
    """Harmonic source with a 900 Hz envelope peak and a 120 Hz F0."""
    t = np.arange(SR * 2, dtype=np.float64) / SR
    signal = np.zeros_like(t)
    for harmonic in range(1, 70):
        frequency = 120.0 * harmonic
        envelope = np.exp(-0.5 * ((frequency - 900.0) / 180.0) ** 2)
        envelope += 0.35 * np.exp(-0.5 * ((frequency - 2200.0) / 300.0) ** 2)
        signal += envelope * np.sin(2.0 * np.pi * frequency * t)
    return 0.25 * signal / np.max(np.abs(signal))


def _dominant_frequency(audio: np.ndarray, lo: float, hi: float) -> float:
    spectrum = np.abs(np.fft.rfft(audio * np.hanning(audio.size)))
    freqs = np.fft.rfftfreq(audio.size, 1.0 / SR)
    mask = (freqs >= lo) & (freqs <= hi)
    return float(freqs[mask][np.argmax(spectrum[mask])])


def test_formant_shifter_preserves_shape_finiteness_and_fundamental():
    source = _vocalish_harmonics()
    shifted = _apply_formant_shifter(source, SR, shift_semitones=7.0)
    assert shifted.shape == source.shape
    assert np.isfinite(shifted).all()
    assert _dominant_frequency(shifted, 100.0, 145.0) == 120.0
    assert not np.allclose(shifted, source)


def test_formant_shifter_supports_mono_stereo_and_dry_mix():
    mono = _vocalish_harmonics()
    stereo = np.stack([mono, mono])
    result = _apply_formant_shifter(stereo, SR, shift_semitones=-5.0)
    assert result.shape == stereo.shape
    np.testing.assert_array_equal(_apply_formant_shifter(mono, SR, shift_semitones=5.0, mix=0.0), mono)


def test_formant_shifter_chain_dispatches_and_clamps():
    mono = _vocalish_harmonics()
    rendered, applied, skipped, _reports, _levels = _run_serial_chain(np.stack([mono, mono]), SR, [
        {"effect": "FormantShifter", "parameters": {"shift_semitones": 99.0, "mix": 2.0}},
    ])
    assert applied == ["FormantShifter"]
    assert skipped == []
    assert rendered.shape == (2, mono.size)
    assert np.isfinite(rendered).all()
