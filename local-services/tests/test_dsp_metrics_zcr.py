"""dsp_metrics.zero_crossing_rate is librosa's frame-wise rate without the
numba kernel that segfaulted on Python 3.13 - the numbers must not move."""
import librosa
import numpy as np
import pytest

from khensu_services.analysis.dsp_metrics import zero_crossing_rate


@pytest.mark.parametrize("n", [2048, 3001, 44100])
@pytest.mark.parametrize("dtype", [np.float32, np.float64])
def test_matches_librosa_exactly(n, dtype):
    rng = np.random.default_rng(n)
    y = (rng.standard_normal(n) * 0.3).astype(dtype)
    y[::7] = 0.0
    y[5:50] = -0.0          # negative zero is zero, not a crossing
    y[100:200] = 1e-12      # under the threshold
    assert np.array_equal(zero_crossing_rate(y), librosa.feature.zero_crossing_rate(y)[0])


def test_multichannel_matches_librosa():
    y = np.random.default_rng(3).standard_normal((2, 5000)).astype(np.float32)
    assert np.array_equal(zero_crossing_rate(y), librosa.feature.zero_crossing_rate(y)[:, 0, :])


def test_sine_rate():
    sr, f = 22050, 441.0
    y = np.sin(2 * np.pi * f * np.arange(sr) / sr)
    inner = zero_crossing_rate(y)[4:-4]  # the edge-padded end frames read lower
    assert np.mean(inner) == pytest.approx(2 * f / sr, rel=0.03)
