# local-services/khensu_services/analysis/artifacts/learned_tier.py
"""Learned AI-artifact detector (ONNX) — registry wrapper.

Runs the exported ArtifactUNet+CNN (see ``analysis/learned/``) over the forensic
feature stack and emits an ``ArtifactFinding`` per artifact class whose
calibrated probability clears its threshold. This is the trained counterpart to
``CodecResidualForensicDetector`` (the heuristic tier) and plugs into the same
registry via the same ``scan``/``scan_stem`` contract.

Safety-audit gate (mandatory): numeric confidence is **not exposed** until the
model's ``intervention_validated`` flag is true (backed by a calibration study —
reliability diagram + ECE on the P2.1 corpus). Until then the detector may still
surface a finding as *evidence* (weight fixed, no probability shown). With no
model present it no-ops entirely, so the registry is unchanged when shipping
without weights — the detector is only registered when a model exists (see
``engine.py``).
"""
from __future__ import annotations

import numpy as np

from khensu_services.analysis.artifacts.base import ArtifactFinding, AudioContext
from khensu_services.analysis.context import load_audio

_CLASS_SUMMARY = {
    "rvq_ghosting": "Metallic HF shimmer (RVQ quantization residual)",
    "codec_residue": "Watery HF modulation (codec overlap-add residue)",
    "hf_aliasing": "Grainy 8-12 kHz texture (codec bandwidth aliasing)",
    "hollow_compression": "Thin mid-range (token-domain dynamics)",
}
# Conservative default thresholds; overridden per-class by the model sidecar's
# calibrated "thresholds" when present.
_DEFAULT_THRESH = 0.5


class LearnedArtifactDetector:
    id = "learned_artifact_cnn"
    tier = "learned"

    def __init__(self) -> None:
        from khensu_services.analysis.learned.inference import LearnedArtifactInference
        self._infer = LearnedArtifactInference()

    @staticmethod
    def available() -> bool:
        # Explicit opt-in gate: even with weights present the detector stays
        # unregistered unless KHENSU_ENABLE_LEARNED_ARTIFACT is set. This keeps a
        # synthetic-bootstrap-trained model (intervention_validated=false) out of
        # default production scans and the regression/golden fixtures until a
        # real, validated model is trained.
        import os
        if os.environ.get("KHENSU_ENABLE_LEARNED_ARTIFACT", "").strip().lower() not in ("1", "true", "yes"):
            return False
        from khensu_services.analysis.learned.inference import model_available
        return model_available()

    def unavailable_reason(self) -> str | None:
        """Why the model cannot run (AID-12), or None."""
        return self._infer.unavailable_reason()

    def scan(self, ctx: AudioContext) -> list[ArtifactFinding]:
        return self._scan(ctx.mono, ctx.sr)

    def scan_stem(self, stem_path: str) -> list[ArtifactFinding]:
        try:
            y, sr = load_audio(stem_path, sr=22050, mono=True)
        except Exception:
            return []
        return self._scan(np.asarray(y, dtype=np.float32), int(sr))

    def _scan(self, y: np.ndarray, sr: int) -> list[ArtifactFinding]:
        if y is None or len(y) < sr:
            return []
        probs = self._infer.predict(np.asarray(y, dtype=np.float32), sr)
        if not probs:
            return []
        validated = self._infer.intervention_validated
        thresholds = self._infer._calib.get("thresholds", {}) or {}
        findings: list[ArtifactFinding] = []
        for cls, p in probs.items():
            thr = float(thresholds.get(cls, _DEFAULT_THRESH))
            if p < thr:
                continue
            # Until calibration is validated, do NOT surface the probability;
            # contribute a fixed, modest evidence weight instead.
            evidence = (f"learned model p={p:.2f} (>= {thr:.2f})" if validated
                        else "learned model flagged this class (confidence withheld "
                             "until calibration is validated)")
            findings.append(ArtifactFinding(
                detector=self.id, kind=cls,
                summary=str(_CLASS_SUMMARY.get(cls) or cls),
                evidence=evidence,
                weight=(float(np.clip(p, 0.0, 0.6)) if validated else 0.15),
                services={"other": 0.25},
                # Validated model = provenance-grade evidence; until then a
                # heuristic artifact signature (S1).
                evidence_class="provenance" if validated else "heuristic"))
        return findings
