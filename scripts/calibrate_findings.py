"""Build the findings calibration table from a labelled corpus (CE-TRU-1).

Usage:
    python scripts/calibrate_findings.py MANIFEST.json [--out PATH] [--min-items N]

The manifest is the evaluation-corpus format validated by
``scripts/validate_evaluation_corpus.py`` (khensu_services/evaluation/
corpus_schema.py). Finding labels live under
``labels.perceptual_diagnosis.findings`` as ``{code: true|false}``; optional
``predictions: {code: score}`` per item carry scores for codes the Python
analyzers cannot compute (the desktop's C++ recommendation engine).

For every item the script runs the Python finding producers (fix-at-source
triage and the revision-report measurement checks) on ``audio_path``
(relative paths resolve against the manifest's folder) and merges the
precomputed predictions. For every labelled code it then reports:

* n            - labelled items for the code
* precision    - of the items where the finding fired, the share labelled true
* recall       - of the items labelled true, the share where it fired
* isotonic     - a monotone map from the finding's 0..1 score to the observed
                 probability that it is right (scikit-learn IsotonicRegression),
                 fitted on the train/val items; precision/recall are measured
                 on the test split when it has labels, else on every item (the
                 table says which in ``evaluated_on``).

Honesty guard: the default output is the shipped table
``resources/calibration/findings_calibration.json``. The script refuses to
write it from a manifest that fails ``validate_split`` or whose provenance
marks any item synthetic - a calibration table may only ship when it was built
from real labelled data. Use ``--out`` elsewhere for experiments.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Callable

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "local-services"))

from khensu_services.common.calibration import MIN_ITEMS, TABLE_FORMAT, TABLE_VERSION  # noqa: E402
from khensu_services.evaluation.corpus_schema import (  # noqa: E402
    CorpusItem,
    finding_labels,
    validate_split,
)

DEFAULT_OUT = ROOT / "resources" / "calibration" / "findings_calibration.json"

Scorer = Callable[[str], dict[str, float]]


def python_scores(audio_path: str) -> dict[str, float]:
    """Scores from the Python finding producers for one file: {code: 0..1}."""
    from khensu_services.analysis.source_triage import triage_source
    from khensu_services.collab.revision import measure_file

    scores: dict[str, float] = {}
    triage = triage_source(audio_path, material="mix")
    for f in triage.get("findings", []):
        code = str(f.get("code", ""))
        if code:
            scores[code] = max(scores.get(code, 0.0), float(f.get("severity", 0.0)))
    _, findings = measure_file(audio_path)
    for f in findings:
        scores[f["code"]] = max(scores.get(f["code"], 0.0), float(f["severity"]))
    return scores


def _precision_recall(pairs: list[tuple[float, bool]]) -> tuple[float, float, int, int, int]:
    tp = sum(1 for s, y in pairs if s > 0 and y)
    fp = sum(1 for s, y in pairs if s > 0 and not y)
    fn = sum(1 for s, y in pairs if s <= 0 and y)
    precision = tp / (tp + fp) if tp + fp else 0.0
    recall = tp / (tp + fn) if tp + fn else 0.0
    return precision, recall, tp, fp, fn


def _isotonic(pairs: list[tuple[float, bool]]) -> dict[str, list[float]] | None:
    if len({y for _, y in pairs}) < 2:
        return None   # one class only: no probability curve to fit
    from sklearn.isotonic import IsotonicRegression
    xs = [s for s, _ in pairs]
    ys = [1.0 if y else 0.0 for _, y in pairs]
    iso = IsotonicRegression(y_min=0.0, y_max=1.0, out_of_bounds="clip", increasing=True)
    iso.fit(xs, ys)
    return {"x": [round(float(v), 6) for v in iso.X_thresholds_],
            "y": [round(float(v), 6) for v in iso.y_thresholds_]}


def build_table(manifest: dict[str, Any], manifest_dir: Path, scorer: Scorer = python_scores,
                manifest_bytes: bytes = b"") -> dict[str, Any]:
    items = [CorpusItem(**it) for it in manifest.get("items", [])]
    rows: list[tuple[CorpusItem, dict[str, bool], dict[str, float]]] = []
    skipped: list[str] = []
    for it in items:
        labels = finding_labels(it)
        if not labels:
            continue
        scores = {k: float(v) for k, v in it.predictions.items()}
        audio = Path(it.audio_path)
        if not audio.is_absolute():
            audio = manifest_dir / audio
        if audio.is_file():
            for code, s in scorer(str(audio)).items():
                scores[code] = max(scores.get(code, 0.0), s)
        elif not it.predictions:
            skipped.append(f"{it.item_id}: audio not found and no predictions")
            continue
        rows.append((it, labels, scores))

    test_rows = [r for r in rows if r[0].split == "test"]
    fit_rows = [r for r in rows if r[0].split != "test"] or rows
    eval_rows = test_rows or rows
    codes: dict[str, Any] = {}
    for code in sorted({c for _, labels, _ in rows for c in labels}):
        def pairs_of(rs):
            return [(scores.get(code, 0.0), labels[code]) for _, labels, scores in rs if code in labels]
        eval_pairs = pairs_of(eval_rows)
        fit_pairs = pairs_of(fit_rows)
        precision, recall, tp, fp, fn = _precision_recall(eval_pairs)
        entry: dict[str, Any] = {"n": len(pairs_of(rows)), "n_eval": len(eval_pairs),
                                 "precision": round(precision, 4), "recall": round(recall, 4),
                                 "tp": tp, "fp": fp, "fn": fn,
                                 "positives": sum(1 for _, y in pairs_of(rows) if y)}
        iso = _isotonic(fit_pairs)
        if iso is not None:
            entry["isotonic"] = iso
        codes[code] = entry

    return {
        "format": TABLE_FORMAT,
        "version": TABLE_VERSION,
        "built_at": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "corpus_sha256": hashlib.sha256(manifest_bytes).hexdigest() if manifest_bytes else "",
        "n_items": len(rows),
        "evaluated_on": "test split" if test_rows else "all items (no labelled test split)",
        "min_items_for_calibrated": MIN_ITEMS,
        "skipped": skipped,
        "codes": codes,
    }


def _synthetic(manifest: dict[str, Any]) -> bool:
    return any("synthetic" in str(it.get("provenance", "")).lower() for it in manifest.get("items", []))


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("manifest")
    ap.add_argument("--out", default="")
    args = ap.parse_args(argv[1:])

    manifest_path = Path(args.manifest)
    raw = manifest_path.read_bytes()
    manifest = json.loads(raw.decode("utf-8"))
    out = Path(args.out) if args.out else DEFAULT_OUT

    if out.resolve() == DEFAULT_OUT.resolve():
        check = validate_split([CorpusItem(**it) for it in manifest.get("items", [])])
        if not check["ok"]:
            print("Refusing to write the shipped calibration table from an invalid corpus:",
                  file=sys.stderr)
            for e in check["errors"]:
                print("  - " + e, file=sys.stderr)
            return 1
        if _synthetic(manifest):
            print("Refusing to write the shipped calibration table from synthetic data. "
                  "Use --out for experiments.", file=sys.stderr)
            return 1

    table = build_table(manifest, manifest_path.parent, manifest_bytes=raw)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(table, indent=1), encoding="utf-8")
    calibrated = [c for c, e in table["codes"].items() if e["n"] >= MIN_ITEMS]
    print(f"Wrote {out}: {table['n_items']} labelled item(s), {len(table['codes'])} code(s), "
          f"{len(calibrated)} with n >= {MIN_ITEMS} (shown as calibrated).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
