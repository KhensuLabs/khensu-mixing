"""WavMark watermark detector structure tests.

Loaders are monkeypatched so no model weights are downloaded — we verify the
guarded no-op path, the positive finding shape, and registry membership.
"""
import numpy as np

import khensu_services.analysis.artifacts.wavmark_tier as wm
from khensu_services.analysis.artifacts.base import AudioContext

SR = 44100


def _ctx():
    t = np.arange(SR * 2) / SR
    return AudioContext(file_path="x.wav", mono=(0.3 * np.sin(2 * np.pi * 220 * t)).astype(np.float32), sr=SR)


def test_wavmark_noops_without_model(monkeypatch):
    monkeypatch.setattr(wm, "_model", lambda: None)
    assert wm.WavMarkWatermarkDetector().scan(_ctx()) == []


def test_wavmark_positive_shape(monkeypatch):
    monkeypatch.setattr(wm, "_model", lambda: object())   # non-None sentinel
    import types
    fake = types.SimpleNamespace(
        decode_watermark=lambda model, y, show_progress=False: (np.ones(16, dtype=int), {"results": [1, 2]}))
    monkeypatch.setitem(__import__("sys").modules, "wavmark", fake)
    out = wm.WavMarkWatermarkDetector().scan(_ctx())
    assert len(out) == 1
    assert out[0].kind == "wavmark_watermark"
    assert out[0].detector == "wavmark_watermark"


def test_wavmark_none_payload_is_negative(monkeypatch):
    monkeypatch.setattr(wm, "_model", lambda: object())
    import types
    fake = types.SimpleNamespace(
        decode_watermark=lambda model, y, show_progress=False: (None, {}))
    monkeypatch.setitem(__import__("sys").modules, "wavmark", fake)
    assert wm.WavMarkWatermarkDetector().scan(_ctx()) == []


def test_wavmark_registered_in_engine():
    from khensu_services.analysis.artifacts.engine import _DETECTORS
    ids = {getattr(d, "id", "") for d in _DETECTORS}
    assert "wavmark_watermark" in ids
