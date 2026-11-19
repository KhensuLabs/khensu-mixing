"""Training harness for the learned AI-artifact model (offline / not run in CI).

Reads a labeled corpus manifest, trains ArtifactUNet+CNN on the forensic feature
stack, exports the model to ONNX, and computes calibration metadata (temperature
+ expected calibration error) that gates confidence exposure.

This script needs a corpus you supply — see ``DATA_SCHEMA.md``. It is intentionally
NOT imported by the service and NOT exercised in CI (no torch at request time, no
corpus in the repo). Run it by hand once you have data:

    python -m khensu_services.analysis.learned.train \\
        --manifest data/artifacts_manifest.jsonl \\
        --out resources/models/artifact_model.onnx --epochs 30

The manifest is JSONL, one clip per line:
    {"path": "clips/xyz.wav", "labels": {"rvq_ghosting": 1, "codec_residue": 0,
                                          "hf_aliasing": 1, "hollow_compression": 0}}

Only the data-loader (``load_manifest`` / ``ManifestDataset``) is unit-tested;
the training loop requires real audio + torch and is validated by hand.
"""
from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import numpy as np

from khensu_services.analysis.learned.features import extract_forensic_features
from khensu_services.analysis.learned.inference import _WINDOW_FRAMES
from khensu_services.analysis.learned.model import ARTIFACT_CLASSES
from khensu_services.analysis.learned.training_provenance import (
    assert_run_paths_available, build_run_manifest, dataloader_worker_init, seed_everything,
    seeded_dataloader_generator, write_run_manifest, write_selected_data_manifest,
)


@dataclass
class Sample:
    path: str
    labels: np.ndarray   # shape (len(ARTIFACT_CLASSES),) float32 in {0,1}


def load_manifest(manifest_path: str) -> list[Sample]:
    """Parse a JSONL manifest into labeled samples (paths not yet loaded)."""
    samples: list[Sample] = []
    base = Path(manifest_path).resolve().parent
    with open(manifest_path, "r", encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            row = json.loads(line)
            labels = row.get("labels", {})
            vec = np.array([float(labels.get(c, 0)) for c in ARTIFACT_CLASSES], dtype=np.float32)
            p = row["path"]
            if not Path(p).is_absolute():
                p = str(base / p)
            samples.append(Sample(path=p, labels=vec))
    return samples


def _window(feats: np.ndarray) -> np.ndarray:
    t = feats.shape[2]
    if t >= _WINDOW_FRAMES:
        start = (t - _WINDOW_FRAMES) // 2
        return feats[:, :, start:start + _WINDOW_FRAMES]
    reps = int(np.ceil(_WINDOW_FRAMES / max(t, 1)))
    return np.tile(feats, (1, 1, reps))[:, :, :_WINDOW_FRAMES]


def featurize_sample(sample: Sample) -> np.ndarray:
    import librosa
    y, sr = librosa.load(sample.path, sr=22050, mono=True)
    return _window(extract_forensic_features(np.asarray(y, dtype=np.float32), int(sr))).astype(np.float32)


def expected_calibration_error(probs: np.ndarray, labels: np.ndarray, n_bins: int = 10) -> float:
    """Standard ECE over flattened per-class predictions (lower is better)."""
    probs = probs.reshape(-1)
    labels = labels.reshape(-1)
    edges = np.linspace(0.0, 1.0, n_bins + 1)
    ece = 0.0
    for i in range(n_bins):
        lo, hi = edges[i], edges[i + 1]
        m = (probs > lo) & (probs <= hi) if i > 0 else (probs >= lo) & (probs <= hi)
        if not np.any(m):
            continue
        conf = float(probs[m].mean())
        acc = float(labels[m].mean())
        ece += (m.mean()) * abs(acc - conf)
    return float(ece)


def train(manifest: str, out_path: str, epochs: int = 30, lr: float = 1e-3,
          batch_size: int = 8, val_frac: float = 0.2, seed: int = 0) -> dict[str, Any]:
    """Train, export ONNX, and write the calibration sidecar. Returns a summary."""
    import torch
    from torch.utils.data import DataLoader, Dataset

    from khensu_services.analysis.learned.model import ArtifactModel

    assert_run_paths_available(out_path)
    seeds = seed_everything(seed)
    samples = load_manifest(manifest)
    if len(samples) < 4:
        raise ValueError("manifest too small — need a real labeled corpus")

    class _DS(Dataset):
        def __init__(self, items): self.items = items
        def __len__(self): return len(self.items)
        def __getitem__(self, i):
            s = self.items[i]
            return torch.from_numpy(featurize_sample(s)), torch.from_numpy(s.labels)

    rng = np.random.default_rng(seed)
    idx = rng.permutation(len(samples))
    n_val = max(1, int(len(samples) * val_frac))
    val = [samples[i] for i in idx[:n_val]]
    trn = [samples[i] for i in idx[n_val:]]

    model = ArtifactModel().module
    opt = torch.optim.Adam(model.parameters(), lr=lr)
    loss_fn = torch.nn.BCEWithLogitsLoss()
    trn_loader = DataLoader(_DS(trn), batch_size=batch_size, shuffle=True,
                            generator=seeded_dataloader_generator(seed),
                            worker_init_fn=dataloader_worker_init(seed))

    for _ in range(epochs):
        model.train()
        for xb, yb in trn_loader:
            opt.zero_grad()
            loss = loss_fn(model(xb), yb)
            loss.backward()
            opt.step()

    # Validation probabilities for calibration.
    model.eval()
    val_loader = DataLoader(_DS(val), batch_size=batch_size)
    all_logits, all_labels = [], []
    with torch.no_grad():
        for xb, yb in val_loader:
            all_logits.append(model(xb).cpu().numpy())
            all_labels.append(yb.cpu().numpy())
    logits = np.concatenate(all_logits) if all_logits else np.zeros((0, len(ARTIFACT_CLASSES)))
    labels = np.concatenate(all_labels) if all_labels else np.zeros((0, len(ARTIFACT_CLASSES)))
    temperature = _fit_temperature(logits, labels)
    probs = 1.0 / (1.0 + np.exp(-logits / temperature))
    ece = expected_calibration_error(probs, labels)

    per_class_accuracy: dict[str, float | None] = {}
    for i, cls in enumerate(ARTIFACT_CLASSES):
        if labels.shape[0] == 0:
            per_class_accuracy[cls] = None
            continue
        per_class_accuracy[cls] = round(
            float(((probs[:, i] > 0.5) == (labels[:, i] > 0.5)).mean()), 3)
    overall_accuracy = (float(np.mean([v for v in per_class_accuracy.values() if v is not None]))
                        if any(v is not None for v in per_class_accuracy.values()) else None)

    # Export ONNX.
    out = Path(out_path)
    out.parent.mkdir(parents=True, exist_ok=True)
    dummy = torch.zeros((1, 7, 128, _WINDOW_FRAMES), dtype=torch.float32)
    from khensu_services.analysis.learned.onnx_export import legacy_onnx_export

    legacy_onnx_export(model, dummy, str(out), input_names=["features"],
                       output_names=["logits"], opset_version=17,
                       dynamic_axes={"features": {0: "batch"}})

    validation_ids = {id(s) for s in val}
    data_manifest = write_selected_data_manifest(out, [
        {"path": s.path, "labels": s.labels.tolist(),
         "split": "validation" if id(s) in validation_ids else "train"}
        for s in samples
    ])
    run_manifest = build_run_manifest(
        output_model=out, model_name="artifact_model", seeds=seeds,
        data_manifest=data_manifest,
        inputs={"manifest": str(Path(manifest)), "epochs": epochs, "lr": lr,
                "batch_size": batch_size, "val_frac": val_frac},
        preprocessing={"feature_schema": "forensic_features", "sample_rate": 22050,
                       "window_frames": _WINDOW_FRAMES, "artifact_classes": ARTIFACT_CLASSES},
    )
    run_manifest_name = write_run_manifest(out, run_manifest)
    sidecar = {
        "temperature": float(temperature),
        "thresholds": {c: 0.5 for c in ARTIFACT_CLASSES},
        "expected_calibration_error": ece,
        "per_class_accuracy": per_class_accuracy,
        "overall_accuracy": overall_accuracy,
        # Deliberately false: a human study must sign off before confidence is
        # exposed (safety audit). Flip only after review.
        "intervention_validated": False,
        "classes": ARTIFACT_CLASSES,
        "n_train": len(trn), "n_val": len(val),
        "run_manifest": run_manifest_name,
        "data_manifest": data_manifest["filename"],
    }
    out.with_suffix(".json").write_text(json.dumps(sidecar, indent=2), encoding="utf-8")
    return {"onnx": str(out), "ece": ece, "temperature": float(temperature),
            "per_class_accuracy": per_class_accuracy, "overall_accuracy": overall_accuracy}


def _fit_temperature(logits: np.ndarray, labels: np.ndarray) -> float:
    """Grid-search a scalar temperature minimizing BCE (simple, dependency-free)."""
    if logits.size == 0:
        return 1.0
    best_t, best_loss = 1.0, float("inf")
    for t in np.linspace(0.5, 3.0, 26):
        p = 1.0 / (1.0 + np.exp(-logits / t))
        p = np.clip(p, 1e-6, 1 - 1e-6)
        loss = float(-np.mean(labels * np.log(p) + (1 - labels) * np.log(1 - p)))
        if loss < best_loss:
            best_loss, best_t = loss, float(t)
    return best_t


if __name__ == "__main__":   # pragma: no cover
    import argparse

    ap = argparse.ArgumentParser(description="Train the learned AI-artifact model.")
    ap.add_argument("--manifest", required=True)
    ap.add_argument("--out", default="resources/models/artifact_model.onnx")
    ap.add_argument("--epochs", type=int, default=30)
    ap.add_argument("--seed", type=int, default=0)
    args = ap.parse_args()
    print(train(args.manifest, args.out, epochs=args.epochs, seed=args.seed))
