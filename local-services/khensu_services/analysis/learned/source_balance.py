"""Source/codec-balanced sampling + per-source calibration for the learned
AI-music detector.

The shipped detector's headline weakness is *source separability*: its real
negatives are MUSDB18-only (studio .mp4) while its AI positives are SONICS mp3,
so it partly learns production/codec rather than a general AI-artifact signal.
The fix has two code-side halves (the third, acquiring diverse real negatives,
is a data task):

1. **Balanced sampling** — even the training set across sources so no single
   source/codec dominates a class and becomes a shortcut feature.
2. **Per-source metrics** — report accuracy + ECE *per source*, so a model that
   only works because MUSDB and SONICS are trivially separable is visible in the
   numbers instead of hiding behind an inflated overall accuracy.
"""
from __future__ import annotations

from typing import Any, Optional, Sequence

import numpy as np


def source_of(clip: Any) -> str:
    """The clip's source tag, falling back to 'unknown'. Clips from the corpus
    builder may not carry one yet; this keeps the pipeline source-aware anyway."""
    return str(getattr(clip, "source", None) or "unknown")


def balance_by_source(clips: Sequence[Any], *, seed: int = 0,
                      per_source: Optional[int] = None) -> list[Any]:
    """Return a subset with each source contributing an equal number of clips.

    Without per_source, every source is downsampled to the smallest source's
    count; with per_source, each is capped at that many (and smaller sources
    contribute all they have). Deterministic given seed.
    """
    rng = np.random.default_rng(seed)
    by_source: dict[str, list[Any]] = {}
    for c in clips:
        by_source.setdefault(source_of(c), []).append(c)
    if not by_source:
        return []
    target = per_source if per_source is not None else min(len(v) for v in by_source.values())

    out: list[Any] = []
    for src in sorted(by_source):
        group = by_source[src]
        k = min(target, len(group))
        idx = rng.choice(len(group), size=k, replace=False)
        out.extend(group[i] for i in idx)
    rng.shuffle(out)
    return out


def _ece(probs: np.ndarray, labels: np.ndarray, n_bins: int = 10) -> float:
    edges = np.linspace(0.0, 1.0, n_bins + 1)
    e = 0.0
    for i in range(n_bins):
        lo = edges[i]
        m = (probs > lo) & (probs <= edges[i + 1]) if i else (probs >= 0.0) & (probs <= edges[1])
        if m.any():
            e += float(m.mean()) * abs(float(labels[m].mean()) - float(probs[m].mean()))
    return float(e)


def reliability_diagram(probs: np.ndarray, labels: np.ndarray, n_bins: int = 10) -> list[dict[str, Any]]:
    """Per-bin reliability-diagram data (audit AI-detection #2): the plottable
    calibration curve behind the scalar ECE. Each bin reports its confidence
    midpoint, the mean predicted probability, the observed accuracy (fraction
    actually positive), and its sample count - so a caller can plot predicted
    vs. observed and see *where* the model is over/under-confident, not just
    the single aggregate. Empty bins are included with count 0 so the x-axis
    stays regular."""
    probs = np.asarray(probs, dtype=float)
    labels = np.asarray(labels, dtype=float)
    edges = np.linspace(0.0, 1.0, n_bins + 1)
    bins: list[dict[str, Any]] = []
    for i in range(n_bins):
        lo, hi = edges[i], edges[i + 1]
        m = (probs > lo) & (probs <= hi) if i else (probs >= 0.0) & (probs <= hi)
        count = int(m.sum())
        bins.append({
            "bin_lo": round(float(lo), 3),
            "bin_hi": round(float(hi), 3),
            "mean_confidence": round(float(probs[m].mean()), 3) if count else None,
            "observed_accuracy": round(float(labels[m].mean()), 3) if count else None,
            "count": count,
        })
    return bins


def per_source_metrics(probs: np.ndarray, labels: np.ndarray,
                       sources: Sequence[str], n_bins: int = 10,
                       include_reliability: bool = False) -> dict[str, dict[str, Any]]:
    """accuracy + ECE + n, broken out per source. Empty sources are skipped.
    When `include_reliability` is set, each source also carries its
    reliability-diagram bins (audit AI-detection #2)."""
    probs = np.asarray(probs, dtype=float)
    labels = np.asarray(labels, dtype=float)
    src = np.asarray([str(s) for s in sources])
    out: dict[str, dict[str, Any]] = {}
    for s in sorted(set(src.tolist())):
        m = src == s
        if not m.any():
            continue
        p, y = probs[m], labels[m]
        entry: dict[str, Any] = {
            "n": int(m.sum()),
            "accuracy": round(float(((p > 0.5) == (y > 0.5)).mean()), 3),
            "ece": round(_ece(p, y, n_bins), 3),
            "positive_rate": round(float((y > 0.5).mean()), 3),
        }
        if include_reliability:
            entry["reliability_diagram"] = reliability_diagram(p, y, n_bins)
        out[s] = entry
    return out
