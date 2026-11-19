"""Fail-closed calibration reports for a future governed genre classifier.

This module intentionally does not train or enable a classifier. It validates
held-out predictions only after ``corpus_schema.validate_split`` accepts a
provenance-backed manifest. Scores are grouped by genre and source quality so
one easy stratum cannot hide failure elsewhere.
"""
from __future__ import annotations

from collections import defaultdict
from typing import Any

from khensu_services.evaluation.corpus_schema import CorpusItem, validate_split


def calibration_report(items: list[CorpusItem], predictions: dict[str, dict[str, Any]]) -> dict[str, Any]:
    """Return held-out accuracy and ECE; refuse non-test or ungoverned input.

    ``predictions[item_id]`` must contain ``label`` and a calibrated candidate
    ``confidence`` in [0, 1]. This is an evaluation-only input: heuristic rank
    scores are never accepted as confidence.
    """
    split = validate_split(items)
    errors = list(split["errors"])
    test_items = [item for item in items if item.split == "test"]
    if not test_items:
        errors.append("No held-out test items supplied.")
    rows: list[tuple[CorpusItem, str, float, bool]] = []
    for item in test_items:
        pred = predictions.get(item.item_id)
        if not pred:
            errors.append(f"Missing held-out prediction for {item.item_id}.")
            continue
        confidence = pred.get("confidence")
        if not isinstance(confidence, (float, int)) or not 0.0 <= float(confidence) <= 1.0:
            errors.append(f"{item.item_id}: calibrated confidence must be in [0, 1].")
            continue
        rows.append((item, str(pred.get("label", "unknown")), float(confidence),
                     str(pred.get("label", "unknown")) == item.strata["genre"]))
    if errors:
        return {"ready": False, "errors": errors, "overall": None, "by_stratum": {}}

    def _metrics(values: list[tuple[float, bool]]) -> dict[str, float]:
        accuracy = sum(correct for _, correct in values) / len(values)
        # Ten fixed bins make reports comparable; no result is emitted for a
        # bin without examples.
        ece = 0.0
        for bucket in range(10):
            group = [(c, ok) for c, ok in values if min(int(c * 10), 9) == bucket]
            if group:
                ece += len(group) / len(values) * abs(sum(c for c, _ in group) / len(group)
                                                    - sum(ok for _, ok in group) / len(group))
        return {"n": len(values), "accuracy": round(accuracy, 4), "expected_calibration_error": round(ece, 4)}

    overall = _metrics([(confidence, correct) for _, _, confidence, correct in rows])
    by_stratum: dict[str, dict[str, dict[str, float]]] = {}
    for axis in ("genre", "source_quality"):
        grouped: dict[str, list[tuple[float, bool]]] = defaultdict(list)
        for item, _label, confidence, correct in rows:
            grouped[item.strata[axis]].append((confidence, correct))
        by_stratum[axis] = {name: _metrics(values) for name, values in grouped.items()}
    return {"ready": True, "errors": [], "overall": overall, "by_stratum": by_stratum}
