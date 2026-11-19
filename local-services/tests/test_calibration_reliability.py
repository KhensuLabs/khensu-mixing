"""Reliability-diagram data + the calibration ECE gate (audit AI-detection #2)."""
from __future__ import annotations

import numpy as np

from khensu_services.analysis.learned.source_balance import (
    reliability_diagram, per_source_metrics,
)


def test_reliability_diagram_of_a_perfectly_calibrated_predictor():
    # Probabilities that exactly match their observed accuracy: e.g. among the
    # p=0.9 predictions, 90% are positive. A well-calibrated curve should have
    # observed_accuracy ~= mean_confidence in every populated bin.
    rng = np.random.default_rng(0)
    probs = rng.uniform(0.0, 1.0, 20000)
    labels = (rng.uniform(0.0, 1.0, 20000) < probs).astype(float)

    bins = reliability_diagram(probs, labels, n_bins=10)
    assert len(bins) == 10
    for b in bins:
        if b["count"] > 50:
            assert abs(b["observed_accuracy"] - b["mean_confidence"]) < 0.05


def test_reliability_diagram_flags_an_overconfident_predictor():
    # Everything predicted p~0.95 but only half are actually positive: the top
    # bin's observed accuracy (~0.5) is far below its confidence (~0.95).
    n = 4000
    probs = np.full(n, 0.95)
    labels = np.concatenate([np.ones(n // 2), np.zeros(n // 2)])
    bins = reliability_diagram(probs, labels, n_bins=10)
    top = [b for b in bins if b["count"] > 0][-1]
    assert top["mean_confidence"] > 0.9
    assert top["observed_accuracy"] < 0.6  # miscalibrated: over-confident


def test_empty_bins_are_present_with_count_zero():
    probs = np.full(100, 0.5)
    labels = np.zeros(100)
    bins = reliability_diagram(probs, labels, n_bins=10)
    assert len(bins) == 10
    empties = [b for b in bins if b["count"] == 0]
    assert empties
    for b in empties:
        assert b["mean_confidence"] is None
        assert b["observed_accuracy"] is None


def test_per_source_metrics_includes_reliability_when_requested():
    probs = np.array([0.1, 0.2, 0.8, 0.9, 0.4, 0.6])
    labels = np.array([0.0, 0.0, 1.0, 1.0, 0.0, 1.0])
    sources = ["a", "a", "a", "b", "b", "b"]

    without = per_source_metrics(probs, labels, sources)
    assert "reliability_diagram" not in without["a"]

    with_rel = per_source_metrics(probs, labels, sources, include_reliability=True)
    assert "reliability_diagram" in with_rel["a"]
    assert isinstance(with_rel["a"]["reliability_diagram"], list)
    # Scalar metrics unchanged by asking for the diagram.
    assert with_rel["a"]["ece"] == without["a"]["ece"]


def test_ece_gate_logic():
    from khensu_services.analysis.artifacts.learned_ai_detector import _should_expose_number

    # Validated AND well-calibrated -> expose the number.
    assert _should_expose_number(
        {"intervention_validated": True, "ece": 0.05, "max_exposed_ece": 0.10}) is True
    # Validated but over/under-confident (high ECE) -> withhold.
    assert _should_expose_number(
        {"intervention_validated": True, "ece": 0.30, "max_exposed_ece": 0.10}) is False
    # Well-calibrated but not intervention-validated -> withhold.
    assert _should_expose_number(
        {"intervention_validated": False, "ece": 0.01}) is False
    # Missing ECE defaults to "untrustworthy" (1.0) -> withhold.
    assert _should_expose_number({"intervention_validated": True}) is False
