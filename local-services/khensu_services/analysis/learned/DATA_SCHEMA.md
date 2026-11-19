# Learned AI-artifact corpus — data schema & governance

The learned tier (`learned_tier.py` → ONNX) is **inert until trained on a
labeled corpus**. This documents the corpus format and the governance rules the
training run must follow before its output may be trusted.

## Manifest format (JSONL)

One clip per line:

```json
{"path": "clips/track_0001.wav", "labels": {"rvq_ghosting": 1, "codec_residue": 0, "hf_aliasing": 1, "hollow_compression": 0}, "source": "suno_v3", "split_key": "artist_042"}
```

- `path` — audio file (absolute, or relative to the manifest). Any librosa-readable format; decoded to 22.05 kHz mono.
- `labels` — 0/1 per artifact class. Missing keys default to 0. The four classes are fixed (`ARTIFACT_CLASSES` in `model.py`): `rvq_ghosting`, `codec_residue`, `hf_aliasing`, `hollow_compression`.
- `source` *(recommended)* — provenance tag (generator/service or "human"). Used for stratified evaluation, not fed to the model.
- `split_key` *(required for governance)* — an artist/project identifier. **Splits are made on this key, never on the clip**, so no artist appears in more than one of train/val/test.

## Label ontology

The labels are *artifact types*, not generator identities. A clip may carry several. Label from **audible + measured evidence** (the heuristic forensic tier's signatures are a starting point), not from the file's provenance alone — a Suno track need not exhibit every artifact.

## Governance (must hold before `intervention_validated` is flipped)

1. **Artist/project-level splits.** Group by `split_key`; a group is wholly in one split. Prevents the model memorizing an artist instead of an artifact.
2. **Balanced coverage.** Roughly balance positive/negative per class and across sources/genres so the model isn't a source detector in disguise.
3. **Calibration by segment.** Report expected calibration error (ECE) overall *and* per source/quality tier. `train.py` fits a temperature and records ECE; a reliability diagram must be reviewed by a human.
4. **Held-out test set.** Never used for model selection. Final metrics (per-class precision/recall + ECE) come from it.
5. **Confidence stays withheld** (`intervention_validated: false` in the sidecar) until the above are reviewed and signed off. The detector then surfaces evidence only, no numeric probability (see `learned_tier.py`).

## Training

```bash
python -m khensu_services.analysis.learned.train \
    --manifest data/artifacts_manifest.jsonl \
    --out resources/models/artifact_model.onnx --epochs 30
```

Writes `artifact_model.onnx` + `artifact_model.json` (temperature, thresholds,
ECE, `intervention_validated: false`, class order). Drop both into
`resources/models/` (or point `KHENSU_ARTIFACT_MODEL` at the `.onnx`) and the
detector auto-registers on next service start.
