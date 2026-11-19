# local-services/tests/test_translation_thirdoctave.py
"""Tests for the 1/3-octave translation analyzer."""
import numpy as np

from khensu_services.analysis.translation_analyzer import (
    analyze_translation, _third_octave_energy, _device_response_31,
    _THIRD_OCTAVE_BANDS, _THIRD_OCTAVE_CENTERS, _DEVICE_RESPONSE_DB,
)

SR = 44100


def _mix(dur=3.0):
    t = np.arange(int(dur * SR)) / SR
    # Sub + low-mid + presence content so several 1/3-octave bands are active.
    return (0.3 * np.sin(2 * np.pi * 60.0 * t)
            + 0.25 * np.sin(2 * np.pi * 400.0 * t)
            + 0.2 * np.sin(2 * np.pi * 3000.0 * t)).astype(np.float32)


def test_thirty_one_bands():
    assert len(_THIRD_OCTAVE_BANDS) == 31
    assert len(_THIRD_OCTAVE_CENTERS) == 31
    # Bands are ascending and non-overlapping-ish (1/3-octave edges).
    for (lo, hi) in _THIRD_OCTAVE_BANDS:
        assert hi > lo


def test_third_octave_energy_normalized_and_localized():
    e = _third_octave_energy(_mix(), SR)
    assert len(e) == 31
    assert abs(float(e.sum()) - 1.0) < 1e-6
    # The 3 kHz tone should light up the 1/3-octave band centered near 3.15 kHz.
    idx = int(np.argmin(np.abs(np.array(_THIRD_OCTAVE_CENTERS) - 3000.0)))
    assert e[idx] > 0.05


def test_device_response_interpolated_to_31():
    for name in _DEVICE_RESPONSE_DB:
        r = _device_response_31(name)
        assert len(r) == 31
        assert np.all(np.isfinite(r))
    # Phone rolls off the sub: its lowest-band gain is well below its presence gain.
    phone = _device_response_31("phone")
    assert phone[0] < phone[20]


def test_analyze_translation_shape():
    out = analyze_translation(_mix(), SR)
    assert set(out["per_profile"]) == {"earbuds", "phone", "car", "club"}
    for _, p in out["per_profile"].items():
        assert 0.0 <= p["degradation_score"] <= 1.0
        for c in p["new_masking_collisions"]:
            assert len(c["band_a_hz"]) == 2 and len(c["band_b_hz"]) == 2
