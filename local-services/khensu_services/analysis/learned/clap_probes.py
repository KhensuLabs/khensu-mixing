"""CLAP mood and AI-generation probes (NEW-9, rest).

Linear heads on the same CLAP track embedding the genre probe uses (three
10 s windows from the middle, mean-pooled; ``genre_probe.window_embeddings``),
trained by ``scripts/train_clap_probes.py`` into
``resources/models/clap_mood_probe.json`` and ``clap_ai_probe.json``. Each
probe file carries its own held-out figures and limitation; both are
experimental evidence beside the existing detectors, never a verdict.
Without CLAP or without the probe file, ``available()`` is False and callers
omit the field.
"""
from __future__ import annotations

import json
import math
from functools import lru_cache
from pathlib import Path
from typing import Any

import numpy as np

MOOD_FILE = "clap_mood_probe.json"
AI_FILE = "clap_ai_probe.json"


def _model_path(name: str) -> Path:
    from khensu_services.common.model_paths import bundled_model

    return bundled_model(name)


@lru_cache(maxsize=4)
def load_probe(name: str) -> dict[str, Any] | None:
    """The probe JSON, or None when it is absent or malformed."""
    path = _model_path(name)
    if not path.is_file():
        return None
    # A bundled model loads only when it matches its reviewed SHA-256 pin
    # (model_pins.json, fail closed - SHIP-5 / AID-14).
    from khensu_services.analysis.model_manifest import artifact_digest_valid

    if not artifact_digest_valid(path, path.name.split(".", 1)[0]):
        return None
    try:
        p = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None
    if p.get("format") != 1 or len(p.get("scaler_mean", [])) != len(p.get("scaler_scale", [])):
        return None
    return p


def _sigmoid(z: float) -> float:
    return 1.0 / (1.0 + math.exp(-max(-60.0, min(60.0, z))))


def _standardise(probe: dict[str, Any], v: np.ndarray) -> np.ndarray:
    mean = np.asarray(probe["scaler_mean"], dtype=np.float64)
    scale = np.asarray(probe["scaler_scale"], dtype=np.float64)
    scale = np.where(scale == 0, 1.0, scale)
    return (np.asarray(v, dtype=np.float64) - mean) / scale


def _score(head: dict[str, Any], x: np.ndarray) -> float:
    return _sigmoid(float(np.dot(np.asarray(head["coef"], dtype=np.float64), x)) + float(head["intercept"]))


def mood_from_embedding(v: np.ndarray, probe: dict[str, Any]) -> dict[str, Any]:
    """{probabilities: {tag: p}, tags: [tags at or over their threshold]}."""
    x = _standardise(probe, v)
    probs = {t: round(_score(probe["heads"][t], x), 4) for t in probe["tags"] if t in probe["heads"]}
    tags = sorted(t for t, p in probs.items() if p >= float(probe["thresholds"].get(t, 0.5)))
    return {"probabilities": probs, "tags": tags}


def ai_from_embedding(v: np.ndarray, probe: dict[str, Any]) -> dict[str, Any]:
    """{probability, cv_auc, unseen_generator_auc, limitation} - evidence, not a verdict."""
    return {
        "probability": round(_score(probe["head"], _standardise(probe, v)), 4),
        "cv_auc": probe.get("cv_auc"),
        "unseen_generator_auc": probe.get("unseen_generator_auc", {}),
        "unseen_source_false_positive_rate": probe.get("unseen_source_false_positive_rate"),
        "limitation": probe.get("limitation", ""),
    }


def cache_tag(name: str) -> str:
    """Whether (and which) probe a cached result was made with."""
    if not available(name):
        return "none"
    p = load_probe(name) or {}
    return f"{p.get('kind')}:{p.get('format')}:{p.get('trained')}"


def available(name: str) -> bool:
    """The probe file loads and CLAP can run offline. Never loads CLAP. The
    test suite's switch for the CLAP genre probe turns these off too."""
    import os

    if os.environ.get("KHENSU_DISABLE_GENRE_PROBE", "").strip() == "1":
        return False
    if load_probe(name) is None:
        return False
    from khensu_services.analysis.learned import genre_probe

    return genre_probe._clap_ready()  # noqa: SLF001 - the shared CLAP readiness check


def _embedding(path: str) -> np.ndarray | None:
    from khensu_services.analysis.learned.genre_probe import pool, window_embeddings

    w = window_embeddings(path)
    return None if w is None or len(w) == 0 else pool(np.asarray(w), "mean")


def predict_mood(path: str) -> dict[str, Any] | None:
    probe = load_probe(MOOD_FILE)
    if probe is None or not available(MOOD_FILE):
        return None
    v = _embedding(path)
    return None if v is None else mood_from_embedding(v, probe)


def predict_ai(path: str) -> dict[str, Any] | None:
    probe = load_probe(AI_FILE)
    if probe is None or not available(AI_FILE):
        return None
    v = _embedding(path)
    return None if v is None else ai_from_embedding(v, probe)
