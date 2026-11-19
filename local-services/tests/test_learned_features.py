import numpy as np

from khensu_services.analysis.learned.features import (
    extract_forensic_features, N_CHANNELS, N_MELS,
)

SR = 22050


def _tone(freq, dur=2.0, sr=SR):
    t = np.arange(int(dur * sr)) / sr
    return (0.3 * np.sin(2 * np.pi * freq * t)).astype(np.float32)


def test_feature_shape_and_finite():
    feats = extract_forensic_features(_tone(220.0), SR)
    assert feats.ndim == 3
    assert feats.shape[0] == N_CHANNELS
    assert feats.shape[1] == N_MELS
    assert feats.shape[2] > 0
    assert np.all(np.isfinite(feats))
    assert feats.dtype == np.float32


def test_feature_deterministic():
    y = _tone(330.0)
    a = extract_forensic_features(y, SR)
    b = extract_forensic_features(y, SR)
    assert np.allclose(a, b)


def test_feature_handles_short_signal():
    feats = extract_forensic_features(np.zeros(256, dtype=np.float32), SR)
    assert feats.shape[0] == N_CHANNELS
    assert np.all(np.isfinite(feats))
