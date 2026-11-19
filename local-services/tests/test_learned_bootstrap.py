"""Synthetic bootstrap corpus + training pipeline for the learned artifact tier.

The corpus generator runs anywhere (numpy/scipy/soundfile); the train+export
roundtrip needs torch/onnx and is guarded. Confirms the produced model stays
intervention_validated=false and the detector only registers behind the opt-in.
"""
import json

import pytest
import soundfile as sf

from khensu_services.analysis.learned.bootstrap_corpus import generate, SR
from khensu_services.analysis.learned.model import ARTIFACT_CLASSES


def test_corpus_generates_balanced_labels(tmp_path):
    manifest = generate(str(tmp_path), n_per_class=3)
    rows = [json.loads(li) for li in open(manifest, encoding="utf-8") if li.strip()]
    # clean + 4 one-hot classes + multilabel, 3 each.
    assert len(rows) == 3 * (1 + len(ARTIFACT_CLASSES) + 1)
    # Every artifact class appears as a positive at least once.
    for cls in ARTIFACT_CLASSES:
        assert any(r["labels"].get(cls) == 1 for r in rows)
    # A clip and its labels are consistent shape.
    y, sr = sf.read(str(tmp_path / rows[0]["path"]))
    assert sr == SR and y.ndim == 1


def test_detector_opt_in_gate(monkeypatch, tmp_path):
    # Without the opt-in env, the detector is unavailable even if a model exists.
    from khensu_services.analysis.artifacts.learned_tier import LearnedArtifactDetector
    monkeypatch.delenv("KHENSU_ENABLE_LEARNED_ARTIFACT", raising=False)
    assert LearnedArtifactDetector.available() is False


def test_bootstrap_train_export(tmp_path, monkeypatch):
    pytest.importorskip("torch")
    pytest.importorskip("onnx")
    from khensu_services.analysis.learned.make_bootstrap_model import build

    out = tmp_path / "m.onnx"
    summary = build(str(out), epochs=1, n_per_class=4)
    assert out.is_file()
    sidecar = json.loads((tmp_path / "m.json").read_text(encoding="utf-8"))
    assert sidecar["intervention_validated"] is False      # never auto-validated
    assert sidecar["provenance"] == "synthetic_bootstrap"
    assert "ece" in summary
