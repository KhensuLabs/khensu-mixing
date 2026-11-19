# local-services/khensu_services/analysis/artifacts/audioseal_tier.py
"""AudioSeal generative-audio watermark detector (Meta, MIT).

A learned, localized watermark detector: many generative models embed (or can
be probed for) an AudioSeal-style watermark, and AudioSeal returns a per-sample
detection probability. This complements the presence-only stationary-HF-carrier
heuristic in the watermark tier with a real, open detector.

Guarded: if `audioseal`/torch aren't installed, the detector simply returns no
findings (never raises). Presence-only — never claims removal, and its numeric
probability is surfaced as evidence, not fused into an overstated confidence
until calibrated (2026-07-15 safety audit).
"""
from __future__ import annotations

import threading

import numpy as np

from khensu_services.analysis.artifacts.base import ArtifactFinding, AudioContext

# AID-18: concurrent scans used to race the lazy load (two loads, or one scan
# seeing a half-initialised global). One lock guards load + the globals.
_LOAD_LOCK = threading.Lock()
_MODEL = None
_LOAD_FAILED = False
_LOAD_ERROR = ""


def _load_audioseal():
    """Build the 16-bit detector from the pinned, SHA-256-verified checkpoint
    through a restricted unpickler (AUDIT-012) - ``AudioSeal.load_detector``
    would ``torch.load(weights_only=False)`` an unpinned file."""
    from khensu_services.analysis.artifacts.safe_weights import build_audioseal_detector
    from khensu_services.analysis.artifacts.watermark_precache import hf_downloads_allowed
    return build_audioseal_detector(allow_download=hf_downloads_allowed())


def unavailable_reason() -> str | None:
    """Cheap (no load) reason this tier cannot run, or None (AUDIT-078)."""
    import importlib.util
    if _MODEL is not None:
        return None
    if importlib.util.find_spec("audioseal") is None:
        return "audioseal package not installed"
    from khensu_services.analysis.artifacts.watermark_precache import gate_open
    if not gate_open("audioseal"):
        return "model weights not pre-cached (watermark models are opt-in)"
    if _LOAD_FAILED:
        return f"model failed to load: {_LOAD_ERROR}" if _LOAD_ERROR else "model failed to load"
    return None


def _detector():
    global _MODEL, _LOAD_FAILED, _LOAD_ERROR
    if _MODEL is not None or _LOAD_FAILED:
        return _MODEL
    with _LOAD_LOCK:
        if _MODEL is not None or _LOAD_FAILED:
            return _MODEL
        return _load_locked()


def _load_locked():
    global _MODEL, _LOAD_FAILED, _LOAD_ERROR
    # Loading the detector downloads a model from the HF Hub on first use, on
    # the analysis thread - left ungated it hung the service when the download
    # stalled. The gate opens when KHENSU_ENABLE_WATERMARK_MODELS=1 OR the
    # weights are already pre-cached (then the load is fast + offline). Pre-cache
    # via `python -m khensu_services.analysis.artifacts.watermark_precache`.
    from khensu_services.analysis.artifacts.watermark_precache import gate_open, load_watermark_model
    if not gate_open("audioseal"):
        return None
    try:
        # Cached-only when downloads are disabled (audit SEC-02): a cache miss
        # fails closed instead of silently fetching from the HF Hub.
        _MODEL = load_watermark_model("audioseal", _load_audioseal)
        if _MODEL is None:
            _LOAD_FAILED = True
            _LOAD_ERROR = "weights not cached and model downloads are disabled"
    except Exception as exc:
        _LOAD_FAILED = True
        _LOAD_ERROR = str(exc)[:200]
        _MODEL = None
    return _MODEL


class AudioSealWatermarkDetector:
    id = "audioseal_watermark"
    tier = "watermark"

    def unavailable_reason(self) -> str | None:
        return unavailable_reason()

    def scan(self, ctx: AudioContext) -> list[ArtifactFinding]:
        det = _detector()
        if det is None:
            return []
        # AID-12: an inference failure is NOT "checked, clean" - it propagates
        # so the engine records the detector under detector_errors.
        import torch
        from khensu_services.common.torch_runtime import ensure_threads
        ensure_threads()
        y = np.asarray(ctx.mono, dtype=np.float32)
        if len(y) < ctx.sr:
            return []
        # AudioSeal expects 16 kHz; resample if needed.
        sr = ctx.sr
        if sr != 16000:
            import librosa
            y = librosa.resample(y, orig_sr=sr, target_sr=16000)
            sr = 16000
        wav = torch.from_numpy(y).unsqueeze(0).unsqueeze(0)  # (batch, ch, samples)
        with torch.no_grad():
            prob_tensor, _msg = det.detect_watermark(wav, sr)
        prob = float(prob_tensor.mean().item())

        if prob >= 0.5:
            # AID-19: a watermark marks provenance, not necessarily AI
            # generation. Provenance class, but one watermark alone (0.5) never
            # reaches "likely AI" (0.7).
            return [ArtifactFinding(
                detector=self.id, kind="audioseal_watermark",
                summary="Provenance watermark present (AudioSeal)",
                evidence=f"AudioSeal detection probability {prob:.2f} (>=0.50) - a learned audio "
                         "watermark is embedded; it marks provenance, not necessarily AI generation",
                weight=0.5, services={"other": 0.4}, evidence_class="provenance")]
        return []
