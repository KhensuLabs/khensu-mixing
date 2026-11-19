"""Real-data binary AI detector: opt-in gate + default-off registry safety."""
import numpy as np

from khensu_services.analysis.artifacts.learned_ai_detector import LearnedAiDetector
from khensu_services.analysis.artifacts.base import AudioContext

SR = 22050


def _ctx():
    t = np.arange(SR * 2) / SR
    return AudioContext("x.wav", (0.3 * np.sin(2 * np.pi * 220 * t)).astype(np.float32), SR)


def test_unavailable_and_noop_without_opt_in(monkeypatch):
    monkeypatch.delenv("KHENSU_ENABLE_AI_DETECTOR", raising=False)
    assert LearnedAiDetector.available() is False
    assert LearnedAiDetector().scan(_ctx()) == []


def test_not_registered_by_default():
    # Even if ai_detector.onnx exists on disk, it must not join the fast intake
    # registry (opt-in off) — keeps intake fast + golden fixtures stable. It may
    # only ever appear in the learned (deep) group.
    from khensu_services.analysis.artifacts.engine import _FAST_DETECTORS
    assert all(getattr(d, "id", "") != "learned_ai_detector" for d in _FAST_DETECTORS)
