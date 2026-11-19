# local-services/khensu_services/analysis/artifacts/acoustic_attribution.py
"""Per-service acoustic attribution detector (audit AI-detection #4).

Metadata attribution dies with a tag strip. This runs the multi-class
`ai_attribution.onnx` model (see `learned/train_attribution.py`) over the
forensic feature tensor and, when one known service clearly dominates,
emits a finding whose `.services` map carries the acoustic per-service
likelihoods — which the engine's existing noisy-OR attribution fuses in
exactly like a metadata hit, restoring "most likely source" on tag-stripped
files with no response-shape change.

Discipline (mirrors `learned_ai_detector`):
  * registered only when the model is present AND `KHENSU_ENABLE_AI_DETECTOR`
    is set (opt-in; nothing ships weights, so default behaviour is unchanged);
  * attribution is a *candidate acoustic match*, never a certain claim — the
    finding carries only a small confidence weight (the binary detector owns
    AI-vs-human confidence), and it stays silent unless the top class clears a
    margin over the runner-up so a genuine human track isn't force-attributed.
"""
from __future__ import annotations

import os
from pathlib import Path

import numpy as np
from khensu_services.analysis.context import load_audio

from khensu_services.analysis.artifacts.base import (
    KNOWN_SERVICES, SERVICE_LABELS, ArtifactFinding, AudioContext)


def _model_path() -> Path | None:
    from khensu_services.analysis.model_manifest import bundled_model_path
    return bundled_model_path("ai_attribution.onnx", "KHENSU_ATTRIBUTION_MODEL")


def _softmax(logits: np.ndarray) -> np.ndarray:
    z = logits - np.max(logits)
    e = np.exp(z)
    return e / (np.sum(e) + 1e-12)


class AcousticAttributionDetector:
    id = "acoustic_attribution"
    tier = "learned"

    # Emit only when the top class beats the runner-up by this margin — below
    # it the acoustic signature is ambiguous and forcing a service would be a
    # guess (a real human track has no true class here).
    MIN_TOP_PROB = 0.45
    MIN_MARGIN = 0.15

    def __init__(self) -> None:
        from khensu_services.analysis.learned.inference import OnnxModelHandle
        self._handle = OnnxModelHandle("ai_attribution", lambda: _model_path())

    @property
    def _meta(self) -> dict:
        return self._handle.calib

    @staticmethod
    def available() -> bool:
        if os.environ.get("KHENSU_ENABLE_AI_DETECTOR", "").strip().lower() not in ("1", "true", "yes"):
            return False
        mp = _model_path()
        if mp is None:
            return False
        from khensu_services.analysis.model_manifest import artifact_digest_valid
        if not artifact_digest_valid(mp):
            return False
        try:
            import onnxruntime  # noqa: F401
        except Exception:
            return False
        return True

    def unavailable_reason(self) -> str | None:
        """Why the model cannot run (AID-12), or None."""
        return self._handle.unavailable_reason()

    def _ensure(self) -> bool:
        return self._handle.ensure()

    def scan(self, ctx: AudioContext) -> list[ArtifactFinding]:
        return self._scan(ctx.mono, ctx.sr)

    def scan_stem(self, stem_path: str) -> list[ArtifactFinding]:
        try:
            y, sr = load_audio(stem_path, sr=22050, mono=True)
        except Exception:
            return []
        return self._scan(np.asarray(y, dtype=np.float32), int(sr))

    def _scan(self, y: np.ndarray, sr: int) -> list[ArtifactFinding]:
        if not self._ensure() or y is None or len(y) < sr:
            return []
        classes = self._meta.get("classes") or []
        if not classes:
            return []
        # AID-13: 4 s windows like training; mean of per-window softmax.
        # Inference errors propagate to the engine (AID-12).
        outs = self._handle.run_windows(np.asarray(y, dtype=np.float32), sr)
        if any(o.size != len(classes) for o in outs):
            return []
        probs = np.mean(np.stack([_softmax(o) for o in outs]), axis=0)
        order = np.argsort(probs)[::-1]
        top_i = int(order[0])
        top_p = float(probs[top_i])
        runner = float(probs[int(order[1])]) if probs.size > 1 else 0.0
        if top_p < self.MIN_TOP_PROB or (top_p - runner) < self.MIN_MARGIN:
            return []
        top_service = classes[top_i]
        if top_service not in KNOWN_SERVICES:
            top_service = "other"
        # Per-service likelihoods for the engine's attribution fusion: only the
        # classes the model actually knows, mapped to KNOWN_SERVICES ids.
        services = {classes[i]: round(float(probs[i]), 3)
                    for i in range(len(classes)) if classes[i] in KNOWN_SERVICES}
        label = SERVICE_LABELS.get(top_service, top_service)
        return [ArtifactFinding(
            detector=self.id, kind="acoustic_attribution",
            summary=f"Acoustic signature resembles {label}",
            evidence=(f"per-service acoustic model: {label} {top_p * 100:.0f}% "
                      f"(vs next {runner * 100:.0f}%) — candidate match, provenance "
                      "tags absent or unconfirmed; not a certain source claim"),
            # Attribution isn't AI-vs-human confidence (the binary detector owns
            # that); contribute only a small nudge so a lone acoustic match can't
            # by itself read as a high-confidence AI verdict.
            weight=0.15,
            services=services,
            # An acoustic resemblance is never provenance (S1): heuristic class,
            # and the engine caps its vendor likelihoods below "detected".
            evidence_class="heuristic")]
