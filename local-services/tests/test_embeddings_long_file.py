"""STR-1: the hand-built embedding runs long files in chunks of frames
(bounded memory) and gets the same features as one pass."""
import numpy as np

from khensu_services.analysis import embeddings as E


def test_chunked_features_equal_one_pass(monkeypatch):
    sr = 22050
    rng = np.random.default_rng(0)
    t = np.arange(sr * 40) / sr
    y = (0.2 * np.sin(2 * np.pi * 220 * t) * (1 + np.sin(2 * np.pi * 0.5 * t))
         + rng.standard_normal(t.size) * 0.05).astype(np.float32)
    monkeypatch.setattr(E, "_HC_CHUNK_FRAMES", 10 ** 9)
    one = E._FrameFeatures(y, sr)
    monkeypatch.setattr(E, "_HC_CHUNK_FRAMES", 300)  # ~6 chunks
    chunked = E._FrameFeatures(y, sr)
    for name in ("log_mel", "onset", "contrast"):
        assert np.allclose(getattr(one, name), getattr(chunked, name), atol=1e-4), name
    assert float(np.dot(one.vector(), chunked.vector())) > 0.9999
