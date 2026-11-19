# local-services/khensu_services/analysis/router_evidence_graph.py
from __future__ import annotations
import logging
import os
from typing import Any

from fastapi import APIRouter
from khensu_services.common.errors import internal_error, not_found

from khensu_services.analysis.analyzer_responses import EvidenceGraphResponse
from khensu_services.analysis.models import EvidenceGraphRequest
from khensu_services.common.executors import run_blocking
from khensu_services.common.lru import BoundedCache, file_cache_key

_log = logging.getLogger(__name__)
router = APIRouter()

# Triage + artifact scan + tonal symptoms (+ masking): cache per file and
# stem content stamps.
_graph_cache: BoundedCache[tuple, dict[str, Any]] = BoundedCache(max_entries=32)


def _collisions_from_masking(stems: dict[str, str]) -> list[dict[str, Any]]:
    """Turn compute_stem_masking pairs into collision nodes: the worst band per
    stem pair whose overall collision score clears a threshold."""
    from khensu_services.analysis.masking import _MASKING_BANDS, compute_stem_masking
    result = compute_stem_masking(stems)
    out: list[dict[str, Any]] = []
    for p in result.get("pairs", []):
        if float(p.get("collision_score", 0.0)) < 0.5:
            continue
        band_scores = p.get("band_scores") or []
        if not band_scores:
            continue
        idx = max(range(len(band_scores)), key=lambda i: band_scores[i])
        lo, hi = _MASKING_BANDS[idx]
        out.append({"band_hz": (float(lo), float(hi)),
                    "score": float(p["collision_score"]),
                    "stems": (p.get("stem_a", "a"), p.get("stem_b", "b"))})
    return out


def _compute(file_path: str, stems: dict[str, str] | None) -> dict[str, Any]:
    from khensu_services.analysis.artifacts import analyze_artifacts
    from khensu_services.analysis.evidence_graph import (
        build_graph, normalize_artifacts, normalize_masking, normalize_tonal_symptoms,
        normalize_triage)
    from khensu_services.analysis.source_triage import triage_source

    from khensu_services.analysis.context import AnalysisContext

    # AID-20: one shared decode for triage, the artifact scan and the tonal
    # symptoms (the graph used to decode the file three times).
    actx = AnalysisContext(file_path)
    nodes: list[dict[str, Any]] = []
    nodes += normalize_triage(triage_source(file_path, ctx=actx).get("findings", []))
    nodes += normalize_artifacts(analyze_artifacts(file_path, analysis_ctx=actx).get("findings", []))
    try:
        from khensu_services.analysis.extended_analyzer import (
            analyze_harshness, analyze_vocal_clarity)

        # These measurements are whole-mix spectral proxies. Keep the
        # triage/artifact graph useful if an unusual input makes the optional
        # enrichment fail.
        y, sr = actx.load(sr=None, mono=True)
        nodes += normalize_tonal_symptoms(
            analyze_harshness(y, int(sr)), analyze_vocal_clarity(y, int(sr)))
    except Exception:
        _log.exception("Tonal symptoms skipped in evidence graph")
    if stems:
        try:
            nodes += normalize_masking(_collisions_from_masking(stems))
        except Exception:
            _log.exception("Masking collisions skipped in evidence graph")
    return build_graph(nodes)


@router.post("/analyze/evidence_graph", response_model=EvidenceGraphResponse, response_model_exclude_unset=True)
async def analyze_evidence_graph_endpoint(req: EvidenceGraphRequest) -> dict[str, Any]:
    if not os.path.isfile(req.file_path):
        not_found(f"File not found: {req.file_path}")
    stems = {k: v for k, v in (req.stems or {}).items() if os.path.isfile(v)} or None
    ck = file_cache_key({"__mix__": req.file_path, **(stems or {})}, "evidence_graph")
    cached = _graph_cache.get(ck) if ck is not None else None
    if cached is not None:
        return cached
    try:
        data = await run_blocking(_compute, req.file_path, stems)
    except Exception as exc:
        _log.exception("Evidence graph failed")
        internal_error(cause=exc)
    if ck is not None:
        _graph_cache[ck] = data
    return data
