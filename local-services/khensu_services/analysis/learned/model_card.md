# Model card — Learned AI-artifact detector (TEMPLATE)

> Fill this in when a model is actually trained. Until then no model ships and
> the detector no-ops. Do **not** flip `intervention_validated` to true without
> completing the "Evaluation" and "Calibration" sections and a human review.

## Overview
- **Task:** multi-label detection of four AI-generation artifact types
  (`rvq_ghosting`, `codec_residue`, `hf_aliasing`, `hollow_compression`) in a
  mix or stem.
- **Architecture:** ArtifactUNet (bounded-mask codec-residual extractor) →
  ArtifactCNN (compact classifier), over a 7-channel forensic feature stack
  (`features.py`). Exported to ONNX; inference via onnxruntime (no torch at
  request time).
- **Input:** 22.05 kHz mono audio → `(7, 128, 256)` feature window.
- **Output:** four calibrated per-class probabilities.

## Intended use
- Evidence within the artifacts-scan fusion, human-in-the-loop, non-destructive.
- **Not** a standalone "this is AI" verdict; **not** an attribution to a specific
  generator; **not** an automated processing trigger.

## Training data
- Corpus: _<describe: sources, size, class balance, genre coverage>_.
- Splits: artist/project-level (`split_key`), see `DATA_SCHEMA.md`.
- Label ontology: artifact types from audible+measured evidence, not provenance.

## Evaluation (held-out test set)
- Per-class precision / recall / F1: _<fill>_.
- Per-source and per-genre breakdown: _<fill>_.

## Calibration
- Temperature: _<fill>_. Overall ECE: _<fill>_. Per-source ECE: _<fill>_.
- Reliability diagram reviewed by: _<name, date>_.

## Limitations & risks
- Risk of learning "source fingerprint" instead of "artifact" if coverage is
  unbalanced — mitigated by balanced sampling + source-stratified evaluation.
- Generalization to unseen generators is unverified until tested.

## Governance sign-off
- `intervention_validated`: **false** until the above is complete and reviewed.
