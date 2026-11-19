# local-services/khensu_services/analysis/evidence_graph.py
"""Typed evidence graph across analyzers (pipeline #7).

The recommendation surface today is a flat list of findings from independent
analyzers (source-defect triage, AI-artifact tells, stem masking), so the
relationships between them — "the mud at 250 Hz is *caused by* the bass/other
collision there" — are left implicit for the user to infer. This builds those
relationships explicitly as **typed edges** between normalized finding nodes,
and derives short causal chains, so an explanation reads as "A causes B" instead
of two unrelated bullet points.

Edges are conservative and evidence-based:
  * `causes`    — a frequency collision (masking) overlapping another finding's
                  band, or a known cause->symptom kind pair (e.g. clipping ->
                  harshness). Directional.
  * `co_occurs` — two findings whose frequency bands overlap but with no known
                  causal direction. Undirected (emitted once, lo-id -> hi-id).

The rule set is intentionally small and extensible — new analyzers add a
normalizer and, optionally, a `_CAUSAL_RULES` entry.
"""
from __future__ import annotations

from typing import Any

Band = tuple[float, float]

# Known cause->symptom kind pairs (rationale shown to the user). Band overlap is
# NOT required for these — they are mechanism-level relationships.
#
# (AID-20: the old "clipping -> spectral_ceiling" edge was dropped - clipping
# adds energy above the band, it cannot carve a ceiling into it.)
#
# Kinds are grounded in what the three wired normalizers actually emit:
# source-triage codes (clipping, mains_hum, dc_offset, comb_filtering,
# proximity_boom, dropouts, high_noise_floor, integrity), artifact-scan kinds
# (spectral_ceiling, hf_aliasing, codec_residue, ...), and masking_collision.
# `harshness` and `muddiness` are emitted by `normalize_tonal_symptoms()` from
# the existing extended spectral measurements.  They remain deliberately
# conservative proxies: neither node claims to identify a vocal or an
# instrument, only elevated energy in a named frequency band.  (An earlier
# revision keyed the hum rule and its band on "hum", which source-triage never
# emits — its code is "mains_hum" — so the rule was silently dead.)
_CAUSAL_RULES: list[tuple[str, str, str]] = [
    ("clipping", "harshness",
     "hard clipping adds broadband harmonic distortion heard as harshness"),
    ("clipping", "hf_aliasing",
     "clipping harmonics above Nyquist fold back down as inharmonic aliasing"),
    ("proximity_boom", "muddiness",
     "proximity/boom piles up low-mid energy heard as mud"),
    ("comb_filtering", "harshness",
     "comb-filter ripple emphasises narrow upper-mid peaks"),
    ("mains_hum", "muddiness",
     "mains hum and its harmonics add steady low-frequency energy"),
    ("mains_hum", "high_noise_floor",
     "mains hum and its harmonics raise the measured noise floor"),
    ("dc_offset", "clipping",
     "a DC offset shifts the waveform toward one rail, so peaks clip earlier and asymmetrically"),
    ("codec_residue", "spectral_ceiling",
     "lossy encoding truncates the spectrum at the codec cutoff, read as a hard high-frequency ceiling"),
]


def _bands_overlap(a: Band | None, b: Band | None) -> bool:
    if not a or not b:
        return False
    return a[0] < b[1] and b[0] < a[1]


def build_graph(nodes: list[dict[str, Any]]) -> dict[str, Any]:
    """Given normalized finding nodes (each: id, kind, label, source,
    severity, band_hz|None), return {nodes, edges, chains}.

    Nodes are passed through unchanged; edges are derived. `chains` renders each
    `causes` edge as a one-line narrative."""
    by_id = {n["id"]: n for n in nodes}
    edges: list[dict[str, Any]] = []
    seen: set[tuple[str, str]] = set()

    def add(src: str, dst: str, relation: str, rationale: str) -> None:
        if relation == "co_occurs":
            key = (src, dst) if src < dst else (dst, src)
        else:
            key = (src, dst)
        if key in seen or src == dst:
            return
        seen.add(key)
        edges.append({"source": src, "target": dst, "relation": relation, "rationale": rationale})

    # 1) Masking collisions cause any band-overlapping finding.
    collisions = [n for n in nodes if n["kind"] == "masking_collision"]
    for coll in collisions:
        for other in nodes:
            if other is coll or other["kind"] == "masking_collision":
                continue
            if _bands_overlap(coll.get("band_hz"), other.get("band_hz")):
                add(coll["id"], other["id"], "causes",
                    "a frequency collision in this band produces the observed problem")

    # 2) Known cause->symptom kind pairs.
    by_kind: dict[str, list[dict[str, Any]]] = {}
    for n in nodes:
        by_kind.setdefault(n["kind"], []).append(n)
    for cause_kind, symptom_kind, rationale in _CAUSAL_RULES:
        for c in by_kind.get(cause_kind, []):
            for s in by_kind.get(symptom_kind, []):
                add(c["id"], s["id"], "causes", rationale)

    # 3) Remaining band overlaps with no causal direction -> co_occurs.
    for i, a in enumerate(nodes):
        for b in nodes[i + 1:]:
            if _bands_overlap(a.get("band_hz"), b.get("band_hz")):
                add(a["id"], b["id"], "co_occurs",
                    "these findings share a frequency region")

    chains = [
        f"{by_id[e['source']]['label']} → {by_id[e['target']]['label']}: {e['rationale']}"
        for e in edges if e["relation"] == "causes"
    ]
    return {"nodes": nodes, "edges": edges, "chains": chains}


# --- normalizers: heterogeneous analyzer outputs -> graph nodes -------------

# source-triage `code` -> canonical kind used by the rules above.
_TRIAGE_KIND = {"noise_floor": "high_noise_floor", "high_noise_floor": "high_noise_floor"}
# rough frequency band per source-triage code (None = broadband / unbanded).
_TRIAGE_BAND: dict[str, Band] = {
    "proximity_boom": (20.0, 250.0),
    "mains_hum": (40.0, 240.0),  # was keyed "hum" — a code triage never emits
}


def normalize_triage(findings: list[dict[str, Any]]) -> list[dict[str, Any]]:
    out = []
    for i, f in enumerate(findings):
        code = f.get("code", "unknown")
        out.append({
            "id": f"triage:{code}:{i}",
            "kind": _TRIAGE_KIND.get(code, code),
            "label": f.get("title", code),
            "source": "source_triage",
            "severity": round(float(f.get("severity", 0.0)), 3),
            "band_hz": _TRIAGE_BAND.get(code),
        })
    return out


def normalize_artifacts(findings: list[dict[str, Any]]) -> list[dict[str, Any]]:
    """One node per artifact kind (AID-20): the scan reports a whole-file
    finding plus per-range localized copies of the same tell, and zero-weight
    production-style observations; graphing each as its own node duplicated
    symptoms. The strongest finding of each kind becomes the node; zero-weight
    duplicates of a kind that already has a node are dropped."""
    best: dict[str, dict[str, Any]] = {}
    order: list[str] = []
    for f in findings:
        kind = str(f.get("kind", "unknown"))
        w = float(f.get("weight", 0.0) or 0.0)
        if kind not in best:
            best[kind] = f
            order.append(kind)
        elif w > float(best[kind].get("weight", 0.0) or 0.0) or (
                best[kind].get("time_range_sec") and not f.get("time_range_sec")
                and w >= float(best[kind].get("weight", 0.0) or 0.0)):
            best[kind] = f
    out = []
    for i, kind in enumerate(order):
        f = best[kind]
        fr = f.get("freq_range_hz")
        band = (float(fr[0]), float(fr[1])) if fr else None
        out.append({
            "id": f"artifact:{kind}:{i}",
            "kind": kind,
            "label": f.get("summary", kind or "artifact"),
            "source": "artifacts",
            "severity": round(float(f.get("weight", 0.0) or 0.0), 3),
            "band_hz": band,
        })
    return out


def normalize_masking(collisions: list[dict[str, Any]]) -> list[dict[str, Any]]:
    """collisions: [{band_hz:(lo,hi), score, stems:(a,b)}]."""
    out = []
    for i, c in enumerate(collisions):
        band = c.get("band_hz")
        stems = c.get("stems", ("", ""))
        out.append({
            "id": f"masking:{i}",
            "kind": "masking_collision",
            "label": f"{stems[0]}/{stems[1]} collision"
                     + (f" @ {band[0]:.0f}-{band[1]:.0f} Hz" if band else ""),
            "source": "masking",
            "severity": round(float(c.get("score", 0.0)), 3),
            "band_hz": (float(band[0]), float(band[1])) if band else None,
        })
    return out


# The extended analyzer's two whole-mix spectral proxies are useful graph
# symptoms only once they clear these thresholds.  This keeps a normal mix from
# acquiring an alarming node merely because every mix contains some energy in
# the named band.  The bands mirror the measurements in extended_analyzer.py.
_HARSHNESS_NODE_THRESHOLD = 0.35
_MUD_RATIO_NODE_THRESHOLD = 0.35
_MUD_CLARITY_MAX = 0.45


def normalize_tonal_symptoms(harshness: dict[str, Any],
                             vocal_clarity: dict[str, Any]) -> list[dict[str, Any]]:
    """Normalize conservative whole-mix tonal symptoms into graph nodes.

    `analyze_harshness()` measures 2.5–5 kHz energy against the mid band.  The
    vocal-clarity measurement is a spectral proxy, so its low-mid result is
    labelled as such rather than presented as a diagnosis of a vocal stem.
    """
    out: list[dict[str, Any]] = []
    harshness_score = float(harshness.get("score", 0.0))
    if harshness_score >= _HARSHNESS_NODE_THRESHOLD:
        out.append({
            "id": "tonal:harshness:0",
            "kind": "harshness",
            "label": "Elevated upper-mid harshness",
            "source": "tonal_symptoms",
            "severity": round(min(harshness_score, 1.0), 3),
            "band_hz": (2500.0, 5000.0),
        })

    mud_ratio = float(vocal_clarity.get("mud_ratio", 0.0))
    clarity_score = float(vocal_clarity.get("clarity_score", 1.0))
    if mud_ratio >= _MUD_RATIO_NODE_THRESHOLD and clarity_score <= _MUD_CLARITY_MAX:
        # Severity starts at zero at the entry threshold and reaches one at a
        # full-band low-mid proxy; the threshold itself is a gating decision,
        # not evidence of a severe problem.
        severity = (mud_ratio - _MUD_RATIO_NODE_THRESHOLD) / (1.0 - _MUD_RATIO_NODE_THRESHOLD)
        out.append({
            "id": "tonal:muddiness:0",
            "kind": "muddiness",
            "label": "Elevated low-mid muddiness (spectral proxy)",
            "source": "tonal_symptoms",
            "severity": round(min(max(severity, 0.0), 1.0), 3),
            "band_hz": (300.0, 1000.0),
        })
    return out
