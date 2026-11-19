# local-services/khensu_services/analysis/mix_quality.py
"""Self-supervised mix-quality inference (pipeline #9).

Runs the opt-in `mix_quality.onnx` model (see learned/train_mix_quality.py) and
returns P(professional-quality) for a mix. Gated with the same discipline as the
learned AI detector: available only when the model is present AND
`KHENSU_ENABLE_MIX_QUALITY` is set, and the numeric probability is exposed only
when the model is intervention-validated AND its calibration is trustworthy
(measured ECE at/under the ceiling) — otherwise a qualitative band is returned
with the number withheld. Nothing ships a model, so the default response is
`available: false`.
"""
from __future__ import annotations

from khensu_services.analysis.context import load_audio
import json
import math
import os
from pathlib import Path
from typing import Any

import numpy as np


def _model_path() -> Path | None:
    from khensu_services.analysis.model_manifest import bundled_model_path
    return bundled_model_path("mix_quality.onnx", "KHENSU_MIX_QUALITY_MODEL")


def _enabled() -> bool:
    return os.environ.get("KHENSU_ENABLE_MIX_QUALITY", "").strip().lower() in ("1", "true", "yes")


def _should_expose_number(calib: dict) -> bool:
    """Numeric P(quality) only when the model is intervention-validated AND its
    calibration error is at/under the exposure ceiling (audit AI-detection #2)."""
    from khensu_services.analysis.learned.inference import calibration_ece
    validated = bool(calib.get("intervention_validated", False))
    ece = calibration_ece(calib)
    max_ece = float(calib.get("max_exposed_ece", 0.10))
    return validated and ece <= max_ece


def _band(p: float) -> str:
    return "clean" if p >= 0.66 else ("borderline" if p >= 0.33 else "likely-defective")


def _reliability_surface(calib: dict[str, Any]) -> tuple[float | None, float | None, list[dict[str, Any]]]:
    """Return safe, plottable held-out calibration evidence from a sidecar.

    The curve is model-level validation metadata, never evidence about the
    currently analysed file. Invalid or incomplete rows are omitted rather
    than letting a malformed sidecar turn into a misleading UI curve.
    """
    def number(value: Any) -> float | None:
        try:
            out = float(value)
        except (TypeError, ValueError):
            return None
        return out if math.isfinite(out) else None

    ece = number(calib.get("val_ece", calib.get("ece")))
    max_ece = number(calib.get("max_exposed_ece", 0.10))
    if ece is not None and not 0.0 <= ece <= 1.0:
        ece = None
    if max_ece is not None and not 0.0 < max_ece <= 1.0:
        max_ece = None

    raw = calib.get("reliability_diagram")
    if not isinstance(raw, list):
        return ece, max_ece, []
    bins: list[dict[str, Any]] = []
    for row in raw:
        if not isinstance(row, dict):
            continue
        lo, hi = number(row.get("bin_lo")), number(row.get("bin_hi"))
        confidence = number(row.get("mean_confidence"))
        accuracy = number(row.get("observed_accuracy"))
        try:
            count = int(row.get("count", 0))
        except (TypeError, ValueError):
            continue
        if (lo is None or hi is None or not (0.0 <= lo < hi <= 1.0) or count < 0):
            continue
        if count == 0:
            bins.append({"bin_lo": lo, "bin_hi": hi, "mean_confidence": None,
                         "observed_accuracy": None, "count": 0})
        elif (confidence is not None and accuracy is not None and
              0.0 <= confidence <= 1.0 and 0.0 <= accuracy <= 1.0):
            bins.append({"bin_lo": lo, "bin_hi": hi, "mean_confidence": confidence,
                         "observed_accuracy": accuracy, "count": count})
    return ece, max_ece, bins


def analyze_mix_quality(file_path: str, ctx=None) -> dict[str, Any]:
    """Return the mix-quality assessment, or `{available: false, ...}` when the
    model/opt-in isn't present (the default)."""
    if not _enabled():
        return {"available": False, "reason": "opt-in disabled (set KHENSU_ENABLE_MIX_QUALITY=1)"}
    mp = _model_path()
    if mp is None:
        return {"available": False, "reason": "no mix_quality.onnx model present"}
    from khensu_services.analysis.model_manifest import artifact_digest_valid
    if not artifact_digest_valid(mp):
        return {"available": False, "reason": "model artifact digest did not match"}
    try:
        import onnxruntime  # noqa: F401
    except Exception:
        return {"available": False, "reason": "onnxruntime not installed"}

    if ctx is not None:
        y, sr = ctx.load(sr=None, mono=True)
    else:
        y, sr = load_audio(file_path, sr=22050, mono=True)
    from khensu_services.analysis.learned.inference import windowed_features
    from khensu_services.analysis.model_manifest import get_onnx_session
    try:
        # AID-14: one cached session (built under a lock), not one per call.
        # AID-13: 4 s windows like training, logits averaged.
        session = get_onnx_session(mp)
        name = session.get_inputs()[0].name
        logits = [float(np.asarray(session.run(None, {name: f})[0]).reshape(-1)[0])
                  for f in windowed_features(np.asarray(y, dtype=np.float32), int(sr))]
        logit = float(np.mean(logits))
    except Exception as exc:
        return {"available": False, "reason": f"inference failed: {type(exc).__name__}"}

    calib: dict = {}
    sc = mp.with_suffix(".json")
    if sc.is_file():
        try:
            calib = json.loads(sc.read_text(encoding="utf-8"))
        except Exception:
            calib = {}
    temp = float(calib.get("temperature", 1.0)) or 1.0
    p = float(1.0 / (1.0 + np.exp(-logit / temp)))
    expose = _should_expose_number(calib)
    calibration_ece, calibration_ece_limit, reliability_diagram = _reliability_surface(calib)
    out: dict[str, Any] = {
        "available": True,
        "quality_band": _band(p),
        "intervention_validated": bool(calib.get("intervention_validated", False)),
        "defect_kinds": calib.get("defect_kinds", []),
        "calibration_ece": calibration_ece,
        "calibration_ece_limit": calibration_ece_limit,
        "reliability_diagram": reliability_diagram,
    }
    if expose:
        out["quality_probability"] = round(p, 3)
    else:
        out["quality_probability"] = None
        out["number_withheld_reason"] = ("calibration not trustworthy or model not "
                                         "intervention-validated; qualitative band only")
    return out
