# local-services/khensu_services/analysis/artifacts/learned_ai_detector.py
"""Binary AI-generated-music detector (real-data ONNX) — registry wrapper.

Runs the `ai_detector.onnx` model trained on real SONICS (AI) vs MUSDB18 (human)
audio (see `learned/train_sonics.py`) and emits a single "ai_generated" finding
when P(AI) clears the threshold. This is the real-data counterpart to the
heuristic forensic tier + the synthetic-bootstrap 4-class model.

Gated: registered only when `ai_detector.onnx` is present AND
`KHENSU_ENABLE_AI_DETECTOR` is set (see `engine.py`). The model's known
limitation — real negatives are MUSDB18-only, so it partly reflects source/
production, not just AI artifacts — means confidence stays **withheld** until
`intervention_validated:true` in the sidecar (2026-07-15 safety audit): a
positive contributes fixed evidence weight, never a surfaced probability.
"""
from __future__ import annotations

import os
from pathlib import Path

import numpy as np

from khensu_services.analysis.artifacts.base import ArtifactFinding, AudioContext
from khensu_services.analysis.context import load_audio


def _should_expose_number(calib: dict) -> bool:
    """Calibration gate (audit AI-detection #2): expose a numeric P(AI) only
    when the model is BOTH intervention-validated AND its calibration is
    actually trustworthy (measured ECE at/under the exposure ceiling). A model
    that classifies well but is over/under-confident (high ECE) still shouldn't
    put a number in front of the user. The trainer writes ``val_ece`` (AID-13)."""
    from khensu_services.analysis.learned.inference import calibration_ece
    validated = bool(calib.get("intervention_validated", False))
    ece = calibration_ece(calib)
    max_ece = float(calib.get("max_exposed_ece", 0.10))
    return validated and ece <= max_ece


def _model_path() -> Path | None:
    from khensu_services.analysis.model_manifest import bundled_model_path
    return bundled_model_path("ai_detector.onnx", "KHENSU_AI_DETECTOR_MODEL")


class LearnedAiDetector:
    id = "learned_ai_detector"
    tier = "learned"

    def __init__(self) -> None:
        from khensu_services.analysis.learned.inference import OnnxModelHandle
        self._handle = OnnxModelHandle("ai_detector", lambda: _model_path())

    @property
    def _calib(self) -> dict:
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
        y, sr = load_audio(stem_path, sr=22050, mono=True)
        return self._scan(np.asarray(y, dtype=np.float32), int(sr))

    def _scan(self, y: np.ndarray, sr: int) -> list[ArtifactFinding]:
        if y is None or len(y) < sr or not self._ensure():
            return []
        from khensu_services.analysis.learned.inference import calibration_ece
        # AID-13: score 4 s windows like training, average the probabilities.
        # Inference errors propagate (AID-12) - the engine records them.
        temp = float(self._calib.get("temperature", 1.0)) or 1.0
        probs = [1.0 / (1.0 + np.exp(-float(out[0]) / temp))
                 for out in self._handle.run_windows(np.asarray(y, dtype=np.float32), sr)]
        p = float(np.mean(probs))
        thr = float(self._calib.get("threshold", 0.5))
        if p < thr:
            return []
        ece = calibration_ece(self._calib)
        max_ece = float(self._calib.get("max_exposed_ece", 0.10))
        expose_number = _should_expose_number(self._calib)
        evidence = (f"learned AI detector P(AI)={p:.2f} over {len(probs)} window(s) "
                    f"(calibration ECE {ece:.3f})" if expose_number
                    else "learned AI detector flagged this as AI-generated "
                         f"(confidence withheld - model not intervention-validated or "
                         f"calibration ECE {ece:.3f} > {max_ece:.2f})")
        return [ArtifactFinding(
            detector=self.id, kind="ai_generated",
            summary="AI-generated music (learned real-data detector)",
            evidence=evidence,
            weight=(float(np.clip(p, 0.0, 0.6)) if expose_number else 0.2),
            services={"other": 0.3},
            # Only a validated, calibrated model is provenance-grade (S1).
            evidence_class="provenance" if expose_number else "heuristic")]
