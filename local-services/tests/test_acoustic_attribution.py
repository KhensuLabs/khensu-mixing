"""Per-service acoustic attribution (audit AI-detection #4).

Covers the opt-in gate + default-off registry safety (mirroring the binary
detector), the SONICS-generator -> service label mapping and leakage-safe
attribution corpus, and the _scan margin gate + services mapping via a fake
ONNX session (so it runs without torch/onnxruntime in CI).
"""
from __future__ import annotations

import numpy as np

from khensu_services.analysis.artifacts.acoustic_attribution import (
    AcousticAttributionDetector, _softmax)
from khensu_services.analysis.artifacts.base import AudioContext, KNOWN_SERVICES

SR = 22050


def _ctx():
    t = np.arange(SR * 2) / SR
    return AudioContext("x.wav", (0.3 * np.sin(2 * np.pi * 220 * t)).astype(np.float32), SR)


# --- gate + registry safety -------------------------------------------------

def test_unavailable_and_noop_without_opt_in(monkeypatch):
    monkeypatch.delenv("KHENSU_ENABLE_AI_DETECTOR", raising=False)
    assert AcousticAttributionDetector.available() is False
    assert AcousticAttributionDetector().scan(_ctx()) == []


def test_not_registered_by_default():
    # Must not join the fast intake registry — only the deep (learned) group.
    from khensu_services.analysis.artifacts.engine import _FAST_DETECTORS
    assert all(getattr(d, "id", "") != "acoustic_attribution" for d in _FAST_DETECTORS)


# --- softmax ----------------------------------------------------------------

def test_softmax_normalizes_and_is_shift_invariant():
    p = _softmax(np.array([2.0, 1.0, 0.0]))
    assert abs(float(p.sum()) - 1.0) < 1e-6
    assert p[0] > p[1] > p[2]
    shifted = _softmax(np.array([102.0, 101.0, 100.0]))
    assert np.allclose(p, shifted, atol=1e-6)


# --- corpus mapping ---------------------------------------------------------

def test_service_of_fake_maps_known_generators_and_drops_unknown():
    from khensu_services.analysis.learned.sonics_corpus import Clip
    from khensu_services.analysis.learned.train_attribution import _service_of_fake

    assert _service_of_fake(Clip("f", 1, "g", source="sonics_suno")) == "suno"
    assert _service_of_fake(Clip("f", 1, "g", source="sonics_udio")) == "udio"
    assert _service_of_fake(Clip("f", 1, "g", source="sonics_ace_step")) == "ace_step"
    assert _service_of_fake(Clip("f", 1, "g", source="sonics_mystery")) is None
    assert _service_of_fake(Clip("f", 1, "g", source="unknown")) is None


def test_attribution_corpus_is_group_split_and_labelled(monkeypatch):
    import khensu_services.analysis.learned.train_attribution as ta
    from khensu_services.analysis.learned.sonics_corpus import Clip

    fakes = [Clip(f"f{i}.mp3", 1, f"song_{i}",
                  source="sonics_suno" if i % 2 else "sonics_udio") for i in range(10)]
    monkeypatch.setattr(ta, "_fake_clips", lambda *a, **k: fakes, raising=False)
    # _attribution_corpus imports _fake_clips from sonics_corpus at call time.
    import khensu_services.analysis.learned.sonics_corpus as sc
    monkeypatch.setattr(sc, "_fake_clips", lambda *a, **k: fakes)

    train, val, classes = ta._attribution_corpus(n_fake=10, val_frac=0.3, seed=0)
    assert classes == ["suno", "udio"]
    train_groups = {c.group for c, _ in train}
    val_groups = {c.group for c, _ in val}
    assert train_groups.isdisjoint(val_groups)          # leakage-safe
    for _, y in train + val:
        assert 0 <= y < len(classes)


# --- _scan margin gate + services mapping -----------------------------------

class _FakeSession:
    def __init__(self, logits):
        self._logits = np.asarray(logits, dtype=np.float32)

    def get_inputs(self):
        class _I:
            name = "features"
        return [_I()]

    def run(self, _outputs, _feed):
        return [self._logits.reshape(1, -1)]


def _detector_with(logits, classes):
    d = AcousticAttributionDetector()
    d._handle.install(_FakeSession(logits), {"classes": classes})
    return d


def test_scan_emits_attribution_when_a_service_dominates():
    d = _detector_with([5.0, 0.0], ["suno", "udio"])
    out = d._scan(_ctx().mono, SR)
    assert len(out) == 1
    f = out[0]
    assert f.kind == "acoustic_attribution"
    assert "suno" in f.services
    # Top service dominates -> its likelihood is the largest in the map.
    assert f.services["suno"] > f.services["udio"]
    assert all(s in KNOWN_SERVICES for s in f.services)


def test_scan_silent_when_ambiguous():
    # Near-tie logits: margin below MIN_MARGIN -> no forced attribution.
    d = _detector_with([0.05, 0.0], ["suno", "udio"])
    assert d._scan(_ctx().mono, SR) == []


def test_scan_silent_when_logits_shape_mismatches_classes():
    d = _detector_with([1.0, 2.0, 3.0], ["suno", "udio"])
    assert d._scan(_ctx().mono, SR) == []
