# local-services/khensu_services/evaluation/corpus_schema.py
"""Evaluation-corpus manifest schema + leakage-safe split validator.

SCAFFOLD ONLY (2026-07-15 safety audit, P2.1). This does NOT contain a corpus:
building one needs real, licensed audio and *human* expert labels + listening
tests, which cannot be synthesized. What it provides is the contract that a
real corpus must satisfy before any model is trained/calibrated or before
`intervention_validated` may ever be set true:

  * a manifest schema stratified by the axes the audit names, and
  * `validate_split()`, which fails closed on artist/project leakage across
    train/val/test and on missing stratification — so a corpus can't quietly
    ship with the flaws (leakage, unlabeled strata) the audit warns about.

Label kinds are kept separate on purpose: `measurement_truth` (deterministic),
`perceptual_diagnosis` (expert), and `intervention_preference` (matched-level
A/B). A recommendation may only claim `intervention_validated` once backed by
`intervention_preference` labels from this corpus.
"""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any

# Stratification axes every corpus item must be tagged on (audit P2.1).
STRATA_AXES = (
    "genre", "arrangement_density", "loudness_band", "sample_rate",
    "channel_layout", "source_quality", "intentional_distortion",
)

LABEL_KINDS = ("measurement_truth", "perceptual_diagnosis", "intervention_preference")
SPLITS = ("train", "val", "test")
PARENT_GENRES = {
    "electronic", "hip_hop", "pop", "rock", "r_and_b", "jazz", "classical",
    "folk", "metal", "ambient", "experimental", "other",
}
SPECIAL_GENRE_LABELS = {"unknown", "hybrid"}


@dataclass
class CorpusItem:
    item_id: str
    audio_path: str
    provenance: str            # license + origin; never "unknown" for a real corpus
    artist_id: str             # split key — items from one artist/project never cross splits
    project_id: str
    split: str                 # one of SPLITS
    strata: dict[str, str]     # must cover STRATA_AXES
    labels: dict[str, Any] = field(default_factory=dict)  # keyed by LABEL_KINDS
    # CE-TRU-1: optional precomputed finding scores {code: 0..1} for codes the
    # Python analyzers cannot reproduce (the desktop's C++ recommendation
    # engine). scripts/calibrate_findings.py merges them with the scores it
    # computes itself.
    predictions: dict[str, float] = field(default_factory=dict)


# CE-TRU-1 finding labels live under labels["perceptual_diagnosis"]["findings"]
# as {finding_code: true | false}: an expert's call on whether the problem the
# code names is really present in the item. Absent codes are unlabelled (not
# "false"), so a corpus can label a subset of codes.
_FINDING_CODE_CHARS = frozenset("abcdefghijklmnopqrstuvwxyz0123456789_")


def finding_labels(item: CorpusItem) -> dict[str, bool]:
    diag = item.labels.get("perceptual_diagnosis")
    if not isinstance(diag, dict):
        return {}
    raw = diag.get("findings")
    if not isinstance(raw, dict):
        return {}
    return {str(k): v for k, v in raw.items() if isinstance(v, bool)}


def validate_finding_labels(item: CorpusItem) -> list[str]:
    problems: list[str] = []
    diag = item.labels.get("perceptual_diagnosis")
    raw = diag.get("findings") if isinstance(diag, dict) else None
    if raw is None:
        return problems
    if not isinstance(raw, dict):
        return [f"{item.item_id}: perceptual_diagnosis.findings must be an object."]
    for code, value in raw.items():
        if not code or not set(str(code)) <= _FINDING_CODE_CHARS:
            problems.append(f"{item.item_id}: finding code '{code}' must be lowercase snake_case.")
        if not isinstance(value, bool):
            problems.append(f"{item.item_id}: finding '{code}' label must be true or false.")
    for code, score in item.predictions.items():
        if isinstance(score, bool) or not isinstance(score, (int, float)) or not 0.0 <= float(score) <= 1.0:
            problems.append(f"{item.item_id}: prediction '{code}' must be a number in 0..1.")
    return problems


def validate_item(item: CorpusItem) -> list[str]:
    problems: list[str] = []
    if item.split not in SPLITS:
        problems.append(f"{item.item_id}: split '{item.split}' not in {SPLITS}")
    if not item.provenance or item.provenance.lower() == "unknown":
        problems.append(f"{item.item_id}: provenance is required (no 'unknown').")
    missing = [ax for ax in STRATA_AXES if ax not in item.strata]
    if missing:
        problems.append(f"{item.item_id}: missing strata {missing}")
    genre = item.strata.get("genre", "")
    if not genre:
        problems.append(f"{item.item_id}: genre label is required.")
    if genre in SPECIAL_GENRE_LABELS and item.split == "train":
        problems.append(f"{item.item_id}: '{genre}' is an abstention/evaluation label, not a train class.")
    subgenre = item.labels.get("genre", {}).get("subgenre") if isinstance(item.labels.get("genre"), dict) else None
    if subgenre and not isinstance(subgenre, str):
        problems.append(f"{item.item_id}: subgenre must be a string when supplied.")
    problems.extend(validate_finding_labels(item))
    return problems


def validate_split(items: list[CorpusItem]) -> dict[str, Any]:
    """Fail closed on artist/project leakage across splits and on missing
    stratification. Returns {ok, errors, coverage}."""
    errors: list[str] = []
    for it in items:
        errors.extend(validate_item(it))

    # Leakage: an artist or project must live in exactly one split.
    for key_name, getter in (("artist", lambda i: i.artist_id),
                             ("project", lambda i: i.project_id)):
        by_key: dict[str, set[str]] = {}
        for it in items:
            by_key.setdefault(getter(it), set()).add(it.split)
        for key, splits in by_key.items():
            if len(splits) > 1:
                errors.append(f"{key_name} '{key}' leaks across splits {sorted(splits)}.")

    coverage = {ax: sorted({it.strata.get(ax, "?") for it in items}) for ax in STRATA_AXES}
    # A classifier test split must include every parent/genre observed in
    # training. Otherwise a headline score quietly excludes hard classes.
    train_genres = {it.strata.get("genre") for it in items if it.split == "train"}
    test_genres = {it.strata.get("genre") for it in items if it.split == "test"}
    missing_test_genres = sorted(g for g in train_genres - test_genres if g and g not in SPECIAL_GENRE_LABELS)
    if missing_test_genres:
        errors.append(f"test split lacks genres present in train: {missing_test_genres}.")
    return {"ok": len(errors) == 0, "errors": errors, "coverage": coverage,
            "n_items": len(items), "missing_test_genres": missing_test_genres}
