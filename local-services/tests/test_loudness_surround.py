# local-services/tests/test_loudness_surround.py
"""BS.1770-4 layout-aware channel weighting for 5.1 / 7.1 (AUDIT-035)."""
import numpy as np
import pytest

from khensu_services.analysis.loudness import (
    integrated_lufs, loudness_range, short_term_loudness_series,
)

SR = 48000


def _noise(seed=0, dur=5.0, amp=0.1):
    return amp * np.random.default_rng(seed).standard_normal(int(dur * SR))


def test_stereo_bed_in_5_1_matches_stereo():
    lr = np.stack([_noise(1), _noise(2)])
    stereo = integrated_lufs(lr, SR)
    surround = np.zeros((6, lr.shape[1]))
    surround[0], surround[1] = lr
    assert integrated_lufs(surround, SR) == pytest.approx(stereo, abs=1e-6)


@pytest.mark.parametrize("n_ch", [6, 8])
def test_lfe_is_excluded(n_ch):
    base = np.zeros((n_ch, int(5 * SR)))
    base[0], base[1] = _noise(1), _noise(2)
    ref = integrated_lufs(base, SR)
    with_lfe = base.copy()
    with_lfe[3] = _noise(3, amp=0.5)      # loud LFE must not move the reading
    assert integrated_lufs(with_lfe, SR) == pytest.approx(ref, abs=1e-6)


def test_surround_weighting():
    sig = _noise(4)
    front = np.zeros((6, sig.size))
    front[0] = sig
    side = np.zeros((6, sig.size))
    side[4] = sig
    # Ls weighted 1.41 -> +1.49 dB vs the same signal in L.
    assert integrated_lufs(side, SR) - integrated_lufs(front, SR) == pytest.approx(
        10 * np.log10(1.41), abs=0.01)
    rear = np.zeros((8, sig.size))
    rear[6] = sig                         # 7.1 rear surround: weight 1.0
    assert integrated_lufs(rear, SR) == pytest.approx(integrated_lufs(front, SR), abs=0.01)


def test_short_term_and_lra_accept_surround():
    y = np.zeros((8, int(6 * SR)))
    y[0], y[1] = _noise(1, dur=6), _noise(2, dur=6)
    assert np.isfinite(short_term_loudness_series(y, SR)).all()
    assert loudness_range(y, SR) is not None


def test_unsupported_channel_count_raises():
    with pytest.raises(ValueError):
        integrated_lufs(np.zeros((7, SR)), SR)
