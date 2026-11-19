# local-services/khensu_services/analysis/artifacts/engine.py
"""Artifact-scan orchestration + confidence fusion (spec §5.1)."""
from __future__ import annotations

from typing import Any

import numpy as np

from khensu_services.analysis.artifacts.base import (
    EVIDENCE_CLASSES, EVIDENCE_HEURISTIC, EVIDENCE_OBSERVATION, EVIDENCE_PROVENANCE,
    KNOWN_SERVICES, SERVICE_LABELS, ArtifactFinding, AudioContext, default_evidence_class,
)
from khensu_services.analysis.artifacts.metadata_tier import (
    ByteSignatureDetector, TagSignatureDetector,
)
from khensu_services.analysis.artifacts.spectral_tier import (
    MacroDynamicsDetector, SectionSimilarityDetector,
    SpectralCeilingDetector, UniformNoiseFloorDetector,
)
from khensu_services.analysis.artifacts.watermark_tier import (
    WatermarkPresenceDetector,
)
from khensu_services.analysis.artifacts.audioseal_tier import AudioSealWatermarkDetector
from khensu_services.analysis.context import load_audio
from khensu_services.analysis.artifacts.wavmark_tier import WavMarkWatermarkDetector
from khensu_services.analysis.artifacts.forensic_tier import CodecResidualForensicDetector
from khensu_services.analysis.artifacts.deep_tier import DEEP_STEM_DETECTORS

# Registered detector instances, run in order. Adding a service or a signal
# means appending here (or to metadata_tier's signature table) — the
# response shape and UI don't change. Deep-tier stem detectors live in
# DEEP_STEM_DETECTORS and run only when stems are supplied.
# Fast heuristic tiers only — cheap DSP (each <200 ms), safe to run on the
# synchronous intake pass. These define the default `analyze_artifacts` result.
_FAST_DETECTORS = [
    TagSignatureDetector(),
    ByteSignatureDetector(),
    SpectralCeilingDetector(),
    UniformNoiseFloorDetector(),
    MacroDynamicsDetector(),
    SectionSimilarityDetector(),
    WatermarkPresenceDetector(),         # stationary-HF-carrier heuristic (fast)
    CodecResidualForensicDetector(),     # HPSS/residual artifact signatures
]

# Learned / model-backed tiers — OFF the fast intake path (pipeline finding
# 2026-07-23). The learned watermark models run full-length inference: AudioSeal
# ~10 s and WavMark's blind sliding-window decode is minutes on a real track, so
# running them in the "fast" intake artifact stage made intake crawl whenever
# their weights were pre-cached (`gate_open` opens on cache presence). They now
# run ONLY when a caller explicitly asks for the deep pass (include_learned=True
# on /analyze/artifacts_scan - the AI Detection screen's explicit scan), never
# in intake / the Dashboard auto-scan / render guardrails
# (AID-08).
_LEARNED_DETECTORS = [
    AudioSealWatermarkDetector(),        # learned generative-watermark (guarded)
    WavMarkWatermarkDetector(),          # learned watermark ensemble (guarded)
]

# The learned ArtifactUNet+CNN / binary-AI / acoustic-attribution detectors are
# registered ONLY when their exported models are present (and, for the AI ones,
# KHENSU_ENABLE_AI_DETECTOR is set), so shipping without weights leaves the
# registry — and every golden/regression fixture — untouched. They join the
# deep (learned) group, not the fast intake path.
try:
    from khensu_services.analysis.artifacts.learned_tier import LearnedArtifactDetector
    if LearnedArtifactDetector.available():
        _LEARNED_DETECTORS.append(LearnedArtifactDetector())
except Exception:
    pass

try:
    from khensu_services.analysis.artifacts.learned_ai_detector import LearnedAiDetector
    if LearnedAiDetector.available():
        _LEARNED_DETECTORS.append(LearnedAiDetector())
except Exception:
    pass

try:
    from khensu_services.analysis.artifacts.acoustic_attribution import AcousticAttributionDetector
    if AcousticAttributionDetector.available():
        _LEARNED_DETECTORS.append(AcousticAttributionDetector())
except Exception:
    pass

# Full registry (fast + learned) for introspection/tests. Execution selects the
# subset via analyze_artifacts(include_learned=...); the fast intake path runs
# _FAST_DETECTORS only.
_DETECTORS = _FAST_DETECTORS + _LEARNED_DETECTORS


# Correlated-evidence groups (AUDIT-019). Several heuristics measure the same
# underlying property - a lossy encode trips the bandwidth ceiling AND the
# codec-residual signatures; heavy limiting trips both the constant noise floor
# AND the flat macro-dynamics - so noisy-OR across them double-counts one cause
# into "likely AI". Within a group only the strongest finding counts; groups are
# then noisy-OR fused as (roughly) independent evidence. Kinds not listed fall
# back to their detector id (one group per detector).
_KIND_GROUPS: dict[str, str] = {
    # container provenance (tags / byte signatures / embedded URLs)
    "metadata_tag": "metadata", "byte_signature": "metadata", "embedded_url": "metadata",
    # decoded watermarks: AudioSeal + WavMark together are still ONE kind of
    # evidence ("a provenance watermark is present"), never "likely AI" on
    # their own (AID-19: 0.5 + 0.5 used to fuse to 0.75).
    "audioseal_watermark": "watermark", "wavmark_watermark": "watermark",
    # bandwidth / codec-reconstruction signatures
    "spectral_ceiling": "codec_spectral", "rvq_ghosting": "codec_spectral",
    "hf_aliasing": "codec_spectral", "codec_residue": "codec_spectral",
    "hollow_compression": "codec_spectral", "hpss_residual_ratio": "codec_spectral",
    # level statistics (both fire on a heavily limited master)
    "uniform_noise_floor": "level_statistics", "macro_dynamics": "level_statistics",
    # per-stem drum realism (same onsets measured two ways)
    "drum_timing_grid": "drum_realism", "repeated_hit_variance": "drum_realism",
}

# Verdict thresholds. "likely_ai" needs provenance evidence of at least
# _LIKELY; heuristic-only evidence is capped below it (S1).
_LIKELY = 0.7
_POSSIBLE = 0.35
_HEURISTIC_ONLY_CAP = 0.69
# A non-provenance finding may hint at a vendor ("resembles X") but never
# reach the checklist's "detected" bar (AID-03).
_NON_PROVENANCE_SERVICE_CAP = 0.2
_DETECTED = 0.5

SCORE_LABEL_UNCALIBRATED = "heuristic signal score (uncalibrated)"


def _get(finding: ArtifactFinding | dict[str, Any], key: str, default: Any = None) -> Any:
    if isinstance(finding, dict):
        return finding.get(key, default)
    return getattr(finding, key, default)


def _class_of(finding: ArtifactFinding | dict[str, Any]) -> str:
    cls = _get(finding, "evidence_class") or ""
    return cls if cls in EVIDENCE_CLASSES else default_evidence_class(str(_get(finding, "kind", "")))


def _group_of(finding: ArtifactFinding | dict[str, Any]) -> str:
    kind, det = str(_get(finding, "kind", "") or ""), str(_get(finding, "detector", "") or "")
    return _KIND_GROUPS.get(kind) or f"detector:{det or kind}"


def _weight_of(finding: ArtifactFinding | dict[str, Any]) -> float:
    """Effective fusion weight: production-style observations carry none (S1,
    AID-02) whatever weight a detector reported."""
    if _class_of(finding) == EVIDENCE_OBSERVATION:
        return 0.0
    w = _get(finding, "weight", 0.0)
    return float(np.clip(float(w or 0.0), 0.0, 0.99))


def _fuse_confidence(findings: list[ArtifactFinding] | list[dict[str, Any]]) -> float:
    """Max within each correlated group, noisy-OR across groups: independent
    weak signals still accumulate and one strong metadata hit dominates, but
    one cause measured by several heuristics counts once. In [0, 1). Accepts
    findings or their `to_dict()` form (scan dicts can be re-fused).
    Observations count zero. Heuristic, NOT calibrated against a labelled
    corpus."""
    best: dict[str, float] = {}
    for f in findings:
        g = _group_of(f)
        best[g] = max(best.get(g, 0.0), _weight_of(f))
    p_none = 1.0
    for w in best.values():
        p_none *= (1.0 - w)
    return float(1.0 - p_none)


def fuse_findings(findings: list[dict[str, Any]]) -> float:
    """Public fusion over serialized findings (see `_fuse_confidence`)."""
    return round(_fuse_confidence(findings), 3)


def _scores(findings: list[ArtifactFinding] | list[dict[str, Any]]) -> dict[str, float]:
    prov = [f for f in findings if _class_of(f) == EVIDENCE_PROVENANCE]
    heur = [f for f in findings if _class_of(f) == EVIDENCE_HEURISTIC]
    provenance = _fuse_confidence(prov)
    heuristic = _fuse_confidence(heur)
    combined = _fuse_confidence(prov + heur)
    if provenance <= 0.0:
        combined = min(combined, _HEURISTIC_ONLY_CAP)
    return {"provenance": provenance, "heuristic": heuristic, "combined": combined}


def _verdict(scores: dict[str, float]) -> str:
    """likely_ai only on provenance evidence (S1); heuristic-only evidence
    tops out at possible_ai."""
    if scores["provenance"] >= _LIKELY:
        return "likely_ai"
    if scores["combined"] >= _POSSIBLE or scores["provenance"] > 0.0:
        return "possible_ai"
    return "no_artifacts_found"


def _service_attribution(findings: list[ArtifactFinding]) -> tuple[dict[str, float], dict[str, str]]:
    """Per-service likelihoods (max within a correlated group, noisy-OR across
    groups) and the basis of each: "provenance" when a provenance finding
    names it, "heuristic" when only non-provenance findings hint at it (those
    are capped at 0.2 - never "detected", AID-03), else "none"."""
    best: dict[tuple[str, str], float] = {}
    basis: dict[str, str] = {s: "none" for s in KNOWN_SERVICES}
    for f in findings:
        g = _group_of(f)
        prov = _class_of(f) == EVIDENCE_PROVENANCE
        for svc, p in f.services.items():
            if svc not in KNOWN_SERVICES:
                continue
            p = float(np.clip(p, 0.0, 0.99))
            if not prov:
                p = min(p, _NON_PROVENANCE_SERVICE_CAP)
            if p <= 0.0:
                continue
            key = (svc, g)
            best[key] = max(best.get(key, 0.0), p)
            if prov:
                basis[svc] = "provenance"
            elif basis[svc] == "none":
                basis[svc] = "heuristic"
    p_none = {s: 1.0 for s in KNOWN_SERVICES}
    for (svc, _g), p in best.items():
        p_none[svc] *= (1.0 - p)
    likelihood = {s: round(1.0 - p, 3) for s, p in p_none.items()}
    for svc in KNOWN_SERVICES:
        if basis[svc] != "provenance":
            likelihood[svc] = min(likelihood[svc], _NON_PROVENANCE_SERVICE_CAP)
    return likelihood, basis


def _summary(findings: list[ArtifactFinding], scores: dict[str, float],
             attribution: dict[str, float], basis: dict[str, str],
             not_run: list[dict[str, str]]) -> str:
    weighted = [f for f in findings if _class_of(f) != EVIDENCE_OBSERVATION]
    observations = [f for f in findings if _class_of(f) == EVIDENCE_OBSERVATION]
    lines: list[str] = []
    if not weighted:
        if observations:
            lines.append("None of the checked AI artifacts were found. "
                         f"{len(observations)} production-style observation(s) are listed "
                         "as context only - they are common in human productions.")
        else:
            lines.append("None of the checked AI artifacts were found.")
    else:
        named = {s: p for s, p in attribution.items()
                 if p >= _DETECTED and s != "other" and basis.get(s) == "provenance"}
        top = max(named, key=lambda s: named[s]) if named else None
        if top is not None:
            lines.append(f"Provenance evidence names {SERVICE_LABELS[top]} "
                         f"({attribution[top] * 100:.0f}% attribution likelihood).")
        elif scores["provenance"] > 0.0:
            lines.append("Provenance evidence present (metadata or a decoded watermark), "
                         "but it names no single service.")
        else:
            lines.append("AI-typical artifacts found; no provenance evidence (metadata or a "
                         "decoded watermark) - this is a heuristic reading, not a verdict.")
        lines.append(f"Heuristic signal score (uncalibrated): {scores['combined'] * 100:.0f}% "
                     f"from {len(weighted)} weighted finding(s).")
    shown = [f for f in findings if f.time_range_sec is None or _weight_of(f) > 0.0]
    for f in shown[:4]:
        where = ""
        if f.time_range_sec:
            where = f" (around {f.time_range_sec[0]:.0f}-{f.time_range_sec[1]:.0f}s)"
        elif f.freq_range_hz:
            where = f" (above {f.freq_range_hz[0] / 1000:.1f} kHz)"
        tag = " [observation]" if _class_of(f) == EVIDENCE_OBSERVATION else ""
        lines.append(f"- {f.summary}{where}{tag}: {f.evidence}")
    if len(shown) > 4:
        lines.append(f"+{len(shown) - 4} more finding(s).")
    if not_run:
        lines.append("Not run: " + ", ".join(d["id"] for d in not_run) + ".")
    return "\n".join(lines)


def _temporal_localization(findings: list[ArtifactFinding], duration_sec: float,
                           window_sec: float = 5.0) -> list[dict[str, Any]]:
    """Per-window heuristic signal (audit AI-detection #3, AID-11).

    Buckets the track into `window_sec` windows and fuses, for each window,
    the findings that apply to it: a finding with a `time_range_sec`
    contributes only to windows it overlaps, while a whole-file finding (no
    time range - e.g. a metadata tag) applies to every window as a baseline.
    A whole-file finding whose tell was also localized (same detector + kind,
    e.g. RVQ ghosting with per-run children) is left out of the baseline: its
    localized children carry the same weight scoped to their ranges, so the
    timeline rises where the tell is instead of being flat. Empty when the
    duration is unknown.
    """
    import math

    if duration_sec <= 0:
        return []
    localized_tells = {(f.detector, f.kind) for f in findings if f.time_range_sec is not None}
    n_windows = max(1, int(math.ceil(duration_sec / window_sec)))
    windows: list[dict[str, Any]] = []
    for w in range(n_windows):
        w_start = w * window_sec
        w_end = min(duration_sec, (w + 1) * window_sec)
        applicable = []
        for f in findings:
            tr = f.time_range_sec
            if tr is None:
                if (f.detector, f.kind) in localized_tells:
                    continue
                applicable.append(f)
            elif not (tr[1] <= w_start or tr[0] >= w_end):
                applicable.append(f)
        windows.append({
            "start_sec": round(w_start, 2),
            "end_sec": round(w_end, 2),
            "confidence": round(_scores(applicable)["combined"], 3),
            "finding_count": len(applicable),
        })
    return windows


def _clipping_census(audio: np.ndarray, sr: int) -> dict[str, Any]:
    """Same per-channel full-scale-run rule as quick_stats, plus per-position
    seconds for the artifacts page (spec §5.1). `audio` is (channels, n) or
    mono (n,)."""
    from khensu_services.analysis.quick_stats import _count_clip_events
    count, positions = _count_clip_events(audio)
    duration = audio.shape[-1] / sr if sr else 0.0
    return {
        "event_count": count,
        "event_positions_sec": [round(p * duration, 2) for p in positions],
    }


def analyze_artifacts(file_path: str,
                      stems: dict[str, str] | None = None,
                      analysis_ctx=None,
                      include_learned: bool = False) -> dict[str, Any]:
    """Scan a file for AI-generation artifacts.

    `stems` optionally maps stem names (e.g. "drums", "vocals") to file
    paths; when present, the deep tier's per-stem observations run (spec §5.1
    heavy pass) - unless the stems are an HPSS approximation (their
    separation.json says so, AID-07), which have no per-instrument meaning.

    `analysis_ctx` is an optional shared `AnalysisContext` (pipeline
    enhancement #1); when supplied the mono decode is taken from it instead
    of re-decoding the file.

    `include_learned` runs the learned/model-backed tiers too (learned
    watermark models + any registered learned AI/attribution detectors). It
    defaults to False so the standard path (intake, Dashboard auto-scan,
    render guardrails) stays fast - the learned watermark
    models do full-length inference (AudioSeal ~10 s, WavMark up to ~27 s) and
    must never block those paths. Only an explicit deep scan passes True.

    The result separates provenance evidence from heuristic artifacts and
    production-style observations (S1): `verdict` is "likely_ai" only on
    provenance evidence, `confidence` is the uncalibrated heuristic signal
    score (capped below "likely" without provenance), and `provenance_score` /
    `heuristic_score` / `provenance_found` / `detector_errors` say why.
    """
    import logging
    import librosa

    # Channels are kept for the clipping census: a clip on one channel can be
    # averaged below full scale in the mono downmix and go uncounted.
    if analysis_ctx is not None:
        channels, sr = analysis_ctx.load(sr=None, mono=False)
        mono, _ = analysis_ctx.load(sr=None, mono=True)
    else:
        channels, sr = load_audio(file_path, sr=None, mono=False)
        mono = librosa.to_mono(channels) if channels.ndim > 1 else channels
    # asarray, not astype: the shared decode is already float32, and a copy
    # per scan is a full-length buffer (detectors never write to ctx.mono).
    ctx = AudioContext(file_path=file_path, mono=np.asarray(mono, dtype=np.float32),
                       sr=int(sr))

    detectors = _FAST_DETECTORS + _LEARNED_DETECTORS if include_learned else _FAST_DETECTORS
    findings: list[ArtifactFinding] = []
    detectors_run: list[str] = []
    # AUDIT-078: a gated / missing / failed model never ran, so it must not be
    # listed as run (which reads as "checked: no watermark").
    detectors_not_run: list[dict[str, str]] = []
    # AID-12: detectors that raised (inference error, load failure mid-scan).
    detector_errors: list[str] = []
    log = logging.getLogger(__name__)
    for det in detectors:
        reason = _unavailable_reason(det)
        if reason:
            detectors_not_run.append({"id": det.id, "reason": reason})
            continue
        try:
            found = det.scan(ctx)
        except Exception:
            # A single broken detector must never take the scan down.
            log.exception("Artifact detector %s failed", det.id)
            detectors_not_run.append({"id": det.id, "reason": "detector error (see service log)"})
            detector_errors.append(det.id)
            continue
        # A lazy model load inside scan() can fail; re-check afterwards.
        reason = _unavailable_reason(det)
        if reason:
            detectors_not_run.append({"id": det.id, "reason": reason})
            continue
        detectors_run.append(det.id)
        findings.extend(found)

    # Deep tier (heavy pass): per-stem observations, only when stems are
    # supplied - and never on HPSS "stems" (AID-07).
    import os
    from khensu_services.analysis.separator import METHOD_HPSS, stems_separation_method
    present = {k: v for k, v in (stems or {}).items() if v and os.path.isfile(v)}
    stem_method = stems_separation_method(present) if present else None
    for det, stem_key in DEEP_STEM_DETECTORS:
        stem_path = present.get(stem_key)
        if not stem_path:
            continue
        if stem_method == METHOD_HPSS:
            detectors_not_run.append({
                "id": det.id,
                "reason": "stems are an HPSS approximation (Demucs unavailable); "
                          "per-instrument checks need model-separated stems"})
            continue
        try:
            findings.extend(det.scan_stem(stem_path))
            detectors_run.append(det.id)
        except Exception:
            log.exception("Deep artifact detector %s failed", det.id)
            detectors_not_run.append({"id": det.id, "reason": "detector error (see service log)"})
            detector_errors.append(det.id)

    findings.sort(key=lambda f: (_class_of(f) == EVIDENCE_PROVENANCE, _weight_of(f), f.weight),
                  reverse=True)
    scores = _scores(findings)
    attribution, basis = _service_attribution(findings)

    checklist = [{
        "id": s,
        "label": SERVICE_LABELS[s],
        "likelihood": attribution[s],
        "detected": attribution[s] >= _DETECTED and basis[s] == "provenance",
        "basis": basis[s],
    } for s in KNOWN_SERVICES]

    duration_sec = round(len(ctx.mono) / ctx.sr, 2) if ctx.sr else 0.0

    out: dict[str, Any] = {
        "confidence": round(scores["combined"], 3),
        "verdict": _verdict(scores),
        "findings": [f.to_dict() for f in findings],
        "service_checklist": checklist,
        "summary": _summary(findings, scores, attribution, basis, detectors_not_run),
        "clipping": _clipping_census(channels, ctx.sr),
        "duration_sec": duration_sec,
        # Per-window heuristic signal for a timeline (audit AI-detection #3,
        # AID-11): localized tells raise their own windows only.
        "timeline": _temporal_localization(findings, duration_sec),
        "detectors_run": detectors_run,
        "detectors_not_run": detectors_not_run,
        "detector_errors": detector_errors,
        # AUDIT-019: the fused number is a heuristic aggregate, not a
        # probability calibrated against a labelled human/AI corpus.
        "calibrated": False,
        "score_label": SCORE_LABEL_UNCALIBRATED,
        "provenance_found": scores["provenance"] > 0.0,
        "provenance_score": round(scores["provenance"], 3),
        "heuristic_score": round(scores["heuristic"], 3),
        "observation_count": sum(1 for f in findings if _class_of(f) == EVIDENCE_OBSERVATION),
        "include_learned": bool(include_learned),
    }
    if stem_method is not None:
        out["stem_separation"] = stem_method
    return out


def _unavailable_reason(det) -> str | None:
    fn = getattr(det, "unavailable_reason", None)
    if fn is None:
        return None
    try:
        return fn()
    except Exception:
        return "availability check failed"


def learned_availability() -> tuple:
    """Cheap per-learned-detector availability snapshot, for cache keys
    (AUDIT-078): a result computed while a model was gated must not be served
    after it becomes available (or vice versa)."""
    return tuple((d.id, _unavailable_reason(d) or "ok") for d in _LEARNED_DETECTORS)
