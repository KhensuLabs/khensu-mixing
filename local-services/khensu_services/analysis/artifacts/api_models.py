"""Request/response models for /analyze/artifacts_scan (typed-client contract).

These supersede the ``Artifact*`` models that lived in ``analyzer_responses``
and ``models.ArtifactScanRequest`` (audit 2026-11-06 S1 / AID-05 / AID-08):
same class names - so the generated C++ ``api::fields`` namespaces keep their
names - plus the evidence-class fields the UI needs to stop presenting a
heuristic as a verdict. Only these classes are referenced by routes, so the
OpenAPI schema carries exactly one definition per name.
"""
from __future__ import annotations


from khensu_services.common.api_models import RequestModel, ResponseModel


class ArtifactScanRequest(RequestModel):
    file_path: str
    # Optional stem paths (e.g. {"drums": "...", "vocals": "..."}) enable the
    # deep tier's per-stem observations (spec §5.1 heavy pass).
    stems: dict[str, str] | None = None
    # AID-08: run the learned/model-backed tiers (AudioSeal, WavMark, any
    # enabled learned detector). Off by default - the Dashboard auto-scan and
    # every other caller get the fast pass; only the AI Detection screen's
    # explicit deep scan asks for it.
    include_learned: bool = False


class ArtifactFindingOut(ResponseModel):
    detector: str | None = None
    kind: str | None = None
    summary: str | None = None
    evidence: str | None = None
    weight: float | None = None
    services: dict[str, float] | None = None   # service id -> likelihood
    time_range_sec: list[float] | None = None
    freq_range_hz: list[float] | None = None
    # "provenance" | "heuristic" | "observation" (S1).
    evidence_class: str | None = None
    # What the finding contributes to the verdict/score (`weight` is the
    # detector's signal strength; observations contribute 0).
    effective_weight: float | None = None


class ArtifactServiceCheck(ResponseModel):
    id: str | None = None
    label: str | None = None
    likelihood: float | None = None
    # True only on provenance evidence (metadata naming the service, a
    # validated model) at >= 0.5.
    detected: bool | None = None
    # "provenance" | "heuristic" (a resemblance, capped at 0.2) | "none".
    basis: str | None = None


class ArtifactClippingCensus(ResponseModel):
    event_count: int | None = None
    event_positions_sec: list[float] | None = None


class ArtifactTimelineWindow(ResponseModel):
    start_sec: float | None = None
    end_sec: float | None = None
    confidence: float | None = None
    finding_count: int | None = None


class ArtifactDetectorNotRun(ResponseModel):
    id: str | None = None
    reason: str | None = None


class ArtifactClapProbe(ResponseModel):
    """NEW-9: the CLAP AI-generation probe - experimental evidence beside the
    fused verdict, never part of it."""
    probability: float
    cv_auc: float | None = None
    unseen_generator_auc: dict[str, float] = {}
    unseen_source_false_positive_rate: float | None = None
    limitation: str = ""


class ArtifactScanResponse(ResponseModel):
    # Uncalibrated heuristic signal score (capped below 0.7 without
    # provenance evidence); see `score_label`.
    confidence: float | None = None
    # "likely_ai" (provenance evidence only) | "possible_ai" | "no_artifacts_found"
    verdict: str | None = None
    findings: list[ArtifactFindingOut] | None = None
    service_checklist: list[ArtifactServiceCheck] | None = None
    summary: str | None = None
    clipping: ArtifactClippingCensus | None = None
    duration_sec: float | None = None
    timeline: list[ArtifactTimelineWindow] | None = None
    detectors_run: list[str] | None = None
    detectors_not_run: list[ArtifactDetectorNotRun] | None = None
    # Detector ids that raised during the scan (also listed in
    # detectors_not_run with reason "detector error") - AID-12.
    detector_errors: list[str] | None = None
    calibrated: bool | None = None
    score_label: str | None = None
    provenance_found: bool | None = None
    provenance_score: float | None = None
    heuristic_score: float | None = None
    observation_count: int | None = None
    include_learned: bool | None = None
    # "demucs" | "hpss" | "unknown" - how the supplied stems were made (AID-07).
    stem_separation: str | None = None
    # NEW-9: the CLAP probe's reading (learned scans only, when it can run).
    clap_probe: ArtifactClapProbe | None = None
