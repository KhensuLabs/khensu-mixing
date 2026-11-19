"""Evidence trail report (CE-TRU-3): why each recommendation was made.

Every desktop recommendation carries the measurements behind it - measurement
name, measured value, unit, the threshold it was held to, and where that
threshold came from (the genre norm, the reference track, or an absolute
standard such as -1 dBTP). This module adds each recommendation's confidence
basis (``common/calibration.py``: calibrated with n / precision, or heuristic),
links the recommendations through the evidence graph
(``analysis/evidence_graph.build_graph``: frequency overlaps and known
cause -> symptom pairs), and writes:

* a JSON document with all of it (machine-readable, for archiving); and
* a self-contained HTML page written for a client: plain sentences, the
  numbers in a table, no scripts, no external assets.
"""
from __future__ import annotations

import json
import math
import pathlib
from typing import Any

from pydantic import Field

from khensu_services.common.api_models import RequestModel, ResponseModel

from .generator import _env, strip_control_chars
from .models import ReportTheme

SOURCES = ("genre_norm", "reference", "absolute")


class EvidenceItemIn(RequestModel):
    measurement: str
    value: float
    unit: str = ""
    threshold: float | None = None
    # "above" = a problem when value > threshold, "below" = when value < it,
    # "outside" = outside a window [threshold_low, threshold].
    comparison: str = ""
    threshold_low: float | None = None
    source: str = "absolute"     # one of SOURCES


class EvidenceRecommendationIn(RequestModel):
    id: str = ""
    issue_code: str
    what: str
    suggested_action: str = ""
    severity: float = 0.0        # the desktop's 0..100
    stem_or_bus: str = ""
    freq_lo_hz: float | None = None
    freq_hi_hz: float | None = None
    evidence: list[EvidenceItemIn] = []


class EvidenceReportRequest(RequestModel):
    session_name: str = ""
    file_name: str = ""
    computed_at: str = ""
    genre: str = ""
    reference_name: str = ""
    output_path: str              # .html
    json_path: str = ""           # optional .json beside it
    recommendations: list[EvidenceRecommendationIn]
    theme: ReportTheme = Field(default_factory=ReportTheme)


class EvidenceConfidenceBasis(ResponseModel):
    kind: str                     # "calibrated" | "heuristic"
    n: int | None = None
    precision: float | None = None
    recall: float | None = None
    probability: float | None = None
    corpus_sha256: str | None = None


class EvidenceItemOut(ResponseModel):
    measurement: str
    value: float
    unit: str
    threshold: float | None = None
    threshold_low: float | None = None
    comparison: str
    source: str
    text: str


class EvidenceRecommendationOut(ResponseModel):
    id: str
    issue_code: str
    what: str
    suggested_action: str
    severity: float
    confidence_basis: EvidenceConfidenceBasis
    evidence: list[EvidenceItemOut]


class EvidenceGraphEdge(ResponseModel):
    source: str
    target: str
    relation: str
    rationale: str


class EvidenceReportResponse(ResponseModel):
    saved_path: str
    json_path: str | None = None
    recommendations: list[EvidenceRecommendationOut]
    edges: list[EvidenceGraphEdge]
    chains: list[str]
    calibrated_count: int
    heuristic_count: int


_SOURCE_TEXT = {
    "genre_norm": "the genre norm",
    "reference": "your reference track",
    "absolute": "an absolute standard",
}


def _num(v: float, unit: str) -> str:
    if not math.isfinite(v):
        return "n/a"
    s = f"{v:.0f}" if abs(v) >= 100 else (f"{v:.2f}" if abs(v) < 10 else f"{v:.1f}")
    return f"{s} {unit}".strip()


def _item_text(e: EvidenceItemIn, source: str) -> str:
    measured = _num(e.value, e.unit)
    where = _SOURCE_TEXT.get(source, source)
    if e.threshold is None:
        return f"{e.measurement} measured {measured}."
    if e.comparison == "outside" and e.threshold_low is not None:
        return (f"{e.measurement} measured {measured}; {where} allows "
                f"{_num(e.threshold_low, e.unit)} to {_num(e.threshold, e.unit)}.")
    if e.comparison == "above":
        return f"{e.measurement} measured {measured}, above the {_num(e.threshold, e.unit)} limit from {where}."
    if e.comparison == "below":
        return f"{e.measurement} measured {measured}, below the {_num(e.threshold, e.unit)} floor from {where}."
    return f"{e.measurement} measured {measured} against {_num(e.threshold, e.unit)} from {where}."


def basis_text(basis: dict[str, Any]) -> str:
    if basis.get("kind") == "calibrated":
        s = (f"Calibrated on {basis['n']} labelled tracks: right {basis['precision'] * 100:.0f}% of the "
             f"time it fires, catches {basis['recall'] * 100:.0f}% of real cases.")
        if basis.get("probability") is not None:
            s += f" For this severity, about {basis['probability'] * 100:.0f}% likely to be right."
        return s
    return ("Heuristic: a rule with a threshold set by an engineer. It has not yet been measured "
            "against a labelled set of mixes, so treat it as an informed opinion, not a verdict.")


def build_evidence(req: EvidenceReportRequest) -> dict[str, Any]:
    from khensu_services.analysis.evidence_graph import build_graph
    from khensu_services.common.calibration import confidence_basis

    recs: list[dict[str, Any]] = []
    nodes: list[dict[str, Any]] = []
    for i, r in enumerate(req.recommendations):
        rid = r.id or f"{r.issue_code}:{i}"
        sev = max(0.0, min(100.0, float(r.severity) if math.isfinite(r.severity) else 0.0))
        basis = confidence_basis(r.issue_code, sev / 100.0)
        items = []
        for e in r.evidence:
            if not math.isfinite(e.value):
                continue
            source = e.source if e.source in SOURCES else "absolute"
            item: dict[str, Any] = {"measurement": e.measurement, "value": round(e.value, 4),
                                    "unit": e.unit, "comparison": e.comparison or "",
                                    "source": source, "text": _item_text(e, source)}
            if e.threshold is not None and math.isfinite(e.threshold):
                item["threshold"] = round(e.threshold, 4)
            if e.threshold_low is not None and math.isfinite(e.threshold_low):
                item["threshold_low"] = round(e.threshold_low, 4)
            items.append(item)
        recs.append({"id": rid, "issue_code": r.issue_code, "what": r.what,
                     "suggested_action": r.suggested_action, "severity": round(sev, 1),
                     "confidence_basis": basis, "evidence": items})
        band = None
        if r.freq_lo_hz is not None and r.freq_hi_hz is not None and r.freq_hi_hz > r.freq_lo_hz > 0:
            band = (float(r.freq_lo_hz), float(r.freq_hi_hz))
        nodes.append({"id": rid, "kind": r.issue_code, "label": r.what or r.issue_code,
                      "source": "recommendations", "severity": round(sev / 100.0, 3), "band_hz": band})
    graph = build_graph(nodes)
    calibrated = sum(1 for r in recs if r["confidence_basis"]["kind"] == "calibrated")
    return {"recommendations": recs, "edges": graph["edges"], "chains": graph["chains"],
            "calibrated_count": calibrated, "heuristic_count": len(recs) - calibrated}


def write_evidence_report(req: EvidenceReportRequest) -> dict[str, Any]:
    doc = build_evidence(req)
    meta = {"session_name": req.session_name, "file_name": req.file_name,
            "computed_at": req.computed_at, "genre": req.genre, "reference_name": req.reference_name}
    for r in doc["recommendations"]:
        r["basis_text"] = basis_text(r["confidence_basis"])
    html = _env.get_template("evidence.html.j2").render(
        **strip_control_chars({**meta, **doc, "theme": req.theme.model_dump()}))
    out = pathlib.Path(req.output_path)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(html, encoding="utf-8")
    result: dict[str, Any] = {"saved_path": str(out.resolve())}
    for r in doc["recommendations"]:
        r.pop("basis_text", None)
    if req.json_path:
        jp = pathlib.Path(req.json_path)
        jp.parent.mkdir(parents=True, exist_ok=True)
        jp.write_text(json.dumps({"format": "khensu-evidence-trail", "version": 1, **meta, **doc},
                                 indent=1), encoding="utf-8")
        result["json_path"] = str(jp.resolve())
    return {**result, **doc}
