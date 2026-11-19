# local-services/tests/test_ai_detector_infra.py
"""Tests for the AI-detector training infrastructure: source-balanced sampling,
per-source calibration metrics, and the model card. (The retrain itself needs a
licensed corpus and is out of scope for CI.)"""
from __future__ import annotations

import json
import os
from dataclasses import dataclass

import numpy as np

from khensu_services.analysis.learned import source_balance as sb
from khensu_services.analysis.learned import model_card as mc


@dataclass
class _Clip:
    source: str
    label: float


def test_balance_downsamples_to_smallest_source():
    clips = ([_Clip("musdb", 0.0) for _ in range(10)]
             + [_Clip("sonics_real", 0.0) for _ in range(3)]
             + [_Clip("sonics_fake", 1.0) for _ in range(7)])
    out = sb.balance_by_source(clips, seed=1)
    counts: dict[str, int] = {}
    for c in out:
        counts[c.source] = counts.get(c.source, 0) + 1
    # Every source contributes the same (smallest = 3) count.
    assert set(counts.values()) == {3}
    assert len(out) == 9


def test_balance_caps_per_source():
    clips = [_Clip("a", 0.0) for _ in range(20)] + [_Clip("b", 1.0) for _ in range(20)]
    out = sb.balance_by_source(clips, seed=0, per_source=5)
    counts: dict[str, int] = {}
    for c in out:
        counts[c.source] = counts.get(c.source, 0) + 1
    assert counts == {"a": 5, "b": 5}


def test_balance_is_deterministic():
    clips = [_Clip("a", 0.0) for _ in range(9)] + [_Clip("b", 1.0) for _ in range(4)]
    a = [id(c) for c in sb.balance_by_source(clips, seed=42)]
    b = [id(c) for c in sb.balance_by_source(clips, seed=42)]
    assert a == b


def test_source_of_falls_back():
    assert sb.source_of(object()) == "unknown"
    assert sb.source_of(_Clip("musdb", 0.0)) == "musdb"


def test_per_source_metrics_break_out_by_source():
    probs = np.array([0.9, 0.8, 0.1, 0.2])
    labels = np.array([1.0, 1.0, 0.0, 0.0])
    sources = ["sonics", "sonics", "musdb", "musdb"]
    m = sb.per_source_metrics(probs, labels, sources)
    assert set(m) == {"sonics", "musdb"}
    assert m["sonics"]["n"] == 2 and m["musdb"]["n"] == 2
    assert m["sonics"]["accuracy"] == 1.0 and m["musdb"]["accuracy"] == 1.0


def test_model_card_build_and_write(tmp_path):
    card = mc.build_model_card(
        name="ai_detector", task="binary_ai_generated_music",
        provenance="sonics_musdb_real", val_accuracy=0.98, val_ece=0.03,
        temperature=0.5, n_train=284, n_val=66,
        per_source_metrics={"musdb": {"n": 30, "accuracy": 0.99, "ece": 0.02}},
        limitations=["narrow negatives"])
    assert card["intervention_validated"] is False
    assert "validation_gate" in card
    onnx = tmp_path / "ai_detector.onnx"
    onnx.write_bytes(b"w")
    paths = mc.write_model_card(str(onnx), card)
    assert os.path.isfile(paths["json"]) and os.path.isfile(paths["markdown"])
    loaded = json.loads(open(paths["json"], encoding="utf-8").read())
    assert loaded["metrics"]["per_source"]["musdb"]["accuracy"] == 0.99
    assert mc.to_markdown(card).startswith("# Model card")
