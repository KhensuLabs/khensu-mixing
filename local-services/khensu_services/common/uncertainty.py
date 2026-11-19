# local-services/khensu_services/common/uncertainty.py
"""First-class measurement uncertainty (pipeline #5).

Most analyzers return bare point estimates, so a consumer (the UI, the
recommendation engine) can't tell a rock-solid number from a shaky one and ends
up treating both as ground truth. This is the shared primitive for attaching
confidence to a measurement: the standard `{value, confidence, quality_flag}`
shape, plus the `quality_flag` derivation.

It's adopted ADDITIVELY — analyzers keep their existing bare fields and can
surface a `qualified(...)` view alongside, so nothing already consuming the raw
number breaks. The convention:

  * confidence: float 0..1 (1 = fully trustworthy).
  * quality_flag: "reliable" (>=0.66) | "tentative" (>=0.33) | "unreliable".
    A caller can override the flag when a hard invalidator applies (e.g.
    clipping corrupts a loudness reading regardless of the estimator's own
    confidence).
"""
from __future__ import annotations

from typing import Any

RELIABLE = "reliable"
TENTATIVE = "tentative"
UNRELIABLE = "unreliable"


def quality_flag(confidence: float) -> str:
    """Band a 0..1 confidence into reliable / tentative / unreliable."""
    c = max(0.0, min(1.0, float(confidence)))
    if c >= 0.66:
        return RELIABLE
    if c >= 0.33:
        return TENTATIVE
    return UNRELIABLE


def qualified(value: Any, confidence: float, flag: str | None = None,
              note: str = "") -> dict[str, Any]:
    """Build the standard measurement shape. `flag` overrides the
    confidence-derived quality_flag (for hard invalidators); `note` explains a
    downgrade to the user."""
    conf = max(0.0, min(1.0, float(confidence)))
    return {
        "value": value,
        "confidence": round(conf, 3),
        "quality_flag": flag or quality_flag(conf),
        "note": note,
    }
