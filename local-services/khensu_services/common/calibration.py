"""Calibrated confidence for findings (CE-TRU-1).

A finding is either **calibrated** - its code has a row in
``resources/calibration/findings_calibration.json``, built by
``scripts/calibrate_findings.py`` from a real labelled corpus, giving the
number of labelled items behind it, the measured precision and recall, and an
isotonic map from the finding's 0..1 score to the probability it is right -
or **heuristic**: a rule with a hand-set threshold that nobody has measured.

No table is shipped until one is built from real labelled data, so every
finding honestly says ``heuristic`` today. The table is re-read when its
mtime changes, so a freshly built table is picked up without a restart.
``KHENSU_CALIBRATION_TABLE`` points at a different file (tests).
"""
from __future__ import annotations

import bisect
import json
import math
import os
import threading
from pathlib import Path
from typing import Any

TABLE_FORMAT = "khensu-findings-calibration"
TABLE_VERSION = 1
# A code needs at least this many labelled items before it may claim
# "calibrated"; below it the numbers are too noisy to show.
MIN_ITEMS = 20

_lock = threading.Lock()
_cache: dict[str, Any] | None = None
_cache_key: tuple[str, float | None] | None = None


def default_table_path() -> Path:
    override = os.environ.get("KHENSU_CALIBRATION_TABLE", "").strip()
    if override:
        return Path(override)
    # local-services/khensu_services/common -> repo root -> resources/
    return Path(__file__).resolve().parents[3] / "resources" / "calibration" / "findings_calibration.json"


def _valid_table(data: Any) -> bool:
    if not isinstance(data, dict) or data.get("format") != TABLE_FORMAT:
        return False
    if not isinstance(data.get("version"), int) or data["version"] > TABLE_VERSION:
        return False
    codes = data.get("codes")
    return isinstance(codes, dict)


def load_table() -> dict[str, Any] | None:
    """The calibration table, or None when absent/invalid (=> all heuristic)."""
    global _cache, _cache_key
    path = default_table_path()
    try:
        mtime: float | None = path.stat().st_mtime
    except OSError:
        mtime = None
    key = (str(path), mtime)
    with _lock:
        if _cache_key == key:
            return _cache
        table = None
        if mtime is not None:
            try:
                data = json.loads(path.read_text(encoding="utf-8"))
                table = data if _valid_table(data) else None
            except (OSError, ValueError):
                table = None
        _cache, _cache_key = table, key
        return table


def _isotonic(points: dict[str, Any], score: float) -> float | None:
    xs, ys = points.get("x"), points.get("y")
    if not isinstance(xs, list) or not isinstance(ys, list) or not xs or len(xs) != len(ys):
        return None
    try:
        xs_f = [float(v) for v in xs]
        ys_f = [float(v) for v in ys]
    except (TypeError, ValueError):
        return None
    if score <= xs_f[0]:
        return ys_f[0]
    if score >= xs_f[-1]:
        return ys_f[-1]
    i = bisect.bisect_right(xs_f, score)
    x0, x1, y0, y1 = xs_f[i - 1], xs_f[i], ys_f[i - 1], ys_f[i]
    if x1 == x0:
        return y1
    return y0 + (y1 - y0) * (score - x0) / (x1 - x0)


def confidence_basis(code: str, score: float | None = None) -> dict[str, Any]:
    """``{"kind": "calibrated", n, precision, recall, probability?}`` when the
    table has a usable row for ``code``, else ``{"kind": "heuristic"}``."""
    table = load_table()
    row = (table or {}).get("codes", {}).get(code) if table else None
    if not isinstance(row, dict):
        return {"kind": "heuristic"}
    try:
        n = int(row.get("n", 0))
        precision = float(row["precision"])
        recall = float(row["recall"])
    except (KeyError, TypeError, ValueError):
        return {"kind": "heuristic"}
    if n < MIN_ITEMS or not (math.isfinite(precision) and math.isfinite(recall)):
        return {"kind": "heuristic"}
    out: dict[str, Any] = {"kind": "calibrated", "n": n,
                           "precision": round(precision, 4), "recall": round(recall, 4)}
    if score is not None and math.isfinite(float(score)) and isinstance(row.get("isotonic"), dict):
        p = _isotonic(row["isotonic"], float(score))
        if p is not None:
            out["probability"] = round(min(max(p, 0.0), 1.0), 4)
    if isinstance(table, dict) and isinstance(table.get("corpus_sha256"), str):
        out["corpus_sha256"] = table["corpus_sha256"]
    return out


def table_summary() -> dict[str, Any]:
    """What the desktop needs to label finding cards without its own parser."""
    table = load_table()
    if table is None:
        return {"available": False, "codes": {}}
    codes = {}
    for code in table.get("codes", {}):
        basis = confidence_basis(code)
        if basis["kind"] == "calibrated":
            codes[code] = {"n": basis["n"], "precision": basis["precision"], "recall": basis["recall"]}
    out: dict[str, Any] = {"available": True, "codes": codes}
    if isinstance(table.get("built_at"), str):
        out["built_at"] = table["built_at"]
    if isinstance(table.get("corpus_sha256"), str):
        out["corpus_sha256"] = table["corpus_sha256"]
    return out
