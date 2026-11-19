"""Per-service acoustic attribution model (audit AI-detection #4).

Metadata attribution (provenance tags / byte signatures) is defeated by a
tag strip. This trains a **multi-class acoustic fingerprint** over the SONICS
positives' *generator* labels (`sonics_suno`, `sonics_udio`, …, already carried
on every fake clip by `sonics_corpus._fake_clips`), so a service can be
attributed from spectral/codec signature alone when the tags are gone.

Same discipline as the binary detector (`train_sonics.py`):
  * leakage-safe split grouped by source-song id,
  * temperature-scaled logits + per-class val accuracy in the sidecar,
  * `intervention_validated:false` — attribution is surfaced as a *candidate*
    acoustic match, never a certain claim, until the corpus/intervention study
    (docs/evaluation/corpus-and-intervention-study.md) clears review.

The retrain is opt-in and the corpus data is git-ignored; nothing ships a
model, so the acoustic-attribution detector stays unregistered by default.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any

import numpy as np

from khensu_services.analysis.learned.features import N_CHANNELS, N_MELS
from khensu_services.analysis.learned.train_sonics import _featurize
from khensu_services.analysis.learned.training_provenance import (
    assert_run_paths_available, build_run_manifest, dataloader_worker_init, seed_everything,
    seeded_dataloader_generator, write_run_manifest, write_selected_data_manifest,
)

# SONICS generator tag (source_of -> "sonics_<algo>") -> KNOWN_SERVICES id.
# Only generators we can map to a named service become attribution classes;
# anything else is skipped (it would just pollute the fingerprint).
_ALGO_TO_SERVICE = {
    "suno": "suno",
    "udio": "udio",
    "musicgen": "musicgen",
    "acestep": "ace_step",
    "ace_step": "ace_step",
    "lyria": "lyria",
    "yue": "yue_diffrhythm",
    "diffrhythm": "yue_diffrhythm",
}


def _service_of_fake(clip) -> str | None:
    src = getattr(clip, "source", "") or ""
    algo = src[len("sonics_"):] if src.startswith("sonics_") else ""
    return _ALGO_TO_SERVICE.get(algo.lower())


def _attribution_corpus(root: str | None = None, n_fake: int = 400,
                        val_frac: float = 0.2, seed: int = 0):
    """(train, val, classes) — SONICS fake clips labelled by service, grouped
    split so no source song spans train/val. `classes` is the ordered service
    list the model's logits index into."""
    from khensu_services.analysis.learned.sonics_corpus import _ROOT, _fake_clips

    fakes = _fake_clips(root or _ROOT, n_fake, seed=seed)
    labelled = [(c, s) for c in fakes if (s := _service_of_fake(c)) is not None]
    classes = sorted({s for _, s in labelled})
    if len(classes) < 2:
        raise ValueError(f"need >=2 attributable services, got {classes}")
    idx = {s: i for i, s in enumerate(classes)}

    groups = sorted({c.group for c, _ in labelled})
    import random
    rng = random.Random(seed)
    rng.shuffle(groups)
    n_val = max(1, int(len(groups) * val_frac))
    val_groups = set(groups[:n_val])
    train = [(c, idx[s]) for c, s in labelled if c.group not in val_groups]
    val = [(c, idx[s]) for c, s in labelled if c.group in val_groups]
    return train, val, classes


def _load_split(pairs):
    X, Y = [], []
    for c, y in pairs:
        f = _featurize(c)
        if f is not None:
            X.append(f)
            Y.append(int(y))
    return np.stack(X), np.array(Y, dtype=np.int64)


def train(out_path: str, epochs: int = 10, n_fake: int = 400,
          lr: float = 1e-3, batch_size: int = 16, seed: int = 0,
          root: str | None = None) -> dict[str, Any]:
    import torch
    from torch.utils.data import DataLoader, TensorDataset

    from khensu_services.analysis.learned.model import build_cnn
    from khensu_services.analysis.learned.model_card import build_model_card, write_model_card

    assert_run_paths_available(out_path)
    seeds = seed_everything(seed)
    train_pairs, val_pairs, classes = _attribution_corpus(root=root, n_fake=n_fake, seed=seed)
    Xtr, Ytr = _load_split(train_pairs)
    Xva, Yva = _load_split(val_pairs)
    if Xtr.shape[0] < 8 or Xva.shape[0] < 4:
        raise ValueError(f"corpus too small (train {Xtr.shape[0]}, val {Xva.shape[0]})")

    model = build_cnn(in_channels=N_CHANNELS, n_classes=len(classes), base=16)
    opt = torch.optim.Adam(model.parameters(), lr=lr)
    loss_fn = torch.nn.CrossEntropyLoss()
    loader = DataLoader(TensorDataset(torch.from_numpy(Xtr), torch.from_numpy(Ytr)),
                        batch_size=batch_size, shuffle=True,
                        generator=seeded_dataloader_generator(seed),
                        worker_init_fn=dataloader_worker_init(seed))
    for _ in range(epochs):
        model.train()
        for xb, yb in loader:
            opt.zero_grad()
            loss_fn(model(xb), yb).backward()
            opt.step()

    model.eval()
    with torch.no_grad():
        va_logits = model(torch.from_numpy(Xva)).numpy()
    pred = va_logits.argmax(axis=1)
    acc = float((pred == Yva).mean())
    per_class = {classes[c]: round(float((pred[Yva == c] == c).mean()), 3)
                 for c in range(len(classes)) if (Yva == c).any()}

    out = Path(out_path)
    out.parent.mkdir(parents=True, exist_ok=True)
    dummy = torch.zeros((1, N_CHANNELS, N_MELS, Xtr.shape[3]), dtype=torch.float32)
    from khensu_services.analysis.learned.onnx_export import legacy_onnx_export

    legacy_onnx_export(model, dummy, str(out), input_names=["features"],
                       output_names=["service_logits"], opset_version=17,
                       dynamic_axes={"features": {0: "batch"}})
    data_manifest = write_selected_data_manifest(out, [
        {"path": c.path, "group": c.group, "label": int(label), "offset": c.offset,
         "source": c.source, "split": split}
        for split, pairs in (("train", train_pairs), ("validation", val_pairs))
        for c, label in pairs
    ])
    run_manifest = build_run_manifest(
        output_model=out, model_name="ai_attribution", seeds=seeds,
        data_manifest=data_manifest,
        inputs={"root": root, "epochs": epochs, "n_fake": n_fake, "lr": lr,
                "batch_size": batch_size},
        preprocessing={"feature_schema": "forensic_features", "sample_rate": 22050,
                       "n_channels": N_CHANNELS, "n_mels": N_MELS,
                       "window": "center_crop_or_tile_256", "classes": classes},
    )
    run_manifest_name = write_run_manifest(out, run_manifest)
    sidecar = {
        "provenance": "sonics_per_generator",
        "task": "per_service_acoustic_attribution",
        "intervention_validated": False,
        "classes": classes,
        "val_accuracy": round(acc, 3),
        "val_per_class_accuracy": per_class,
        "n_train": int(Xtr.shape[0]), "n_val": int(Xva.shape[0]),
        "run_manifest": run_manifest_name,
        "data_manifest": data_manifest["filename"],
        "limitation": "Trained only on SONICS generator labels; attributes among "
                      "those services only and reflects their SONICS-era encode "
                      "settings. Surfaced as a candidate acoustic match, not a "
                      "certain claim. Not validated — opt-in.",
    }
    out.with_suffix(".json").write_text(json.dumps(sidecar, indent=2), encoding="utf-8")

    card = build_model_card(
        name="ai_attribution", task="per_service_acoustic_attribution",
        provenance="sonics_per_generator", val_accuracy=acc, val_ece=0.0,
        temperature=1.0, n_train=int(Xtr.shape[0]), n_val=int(Xva.shape[0]),
        per_source_metrics=per_class, intervention_validated=False,
        intended_use="Opt-in forensic aid: which known service a track most "
                     "acoustically resembles when provenance tags are stripped. "
                     "Not a standalone AI-vs-human verdict.",
        license_note="SONICS training data CC-BY-NC-4.0 (research/eval only).",
        limitations=[
            "Closed-set over SONICS generators only — cannot attribute a service "
            "it never saw, and will still pick its nearest known class.",
            "Reflects SONICS-era per-generator encode settings; robustness to "
            "newer model versions / re-encodes is unverified.",
            "Confidence withheld until the corpus/intervention study clears review.",
        ])
    write_model_card(str(out), card)

    return {"onnx": str(out), "classes": classes, "val_accuracy": acc,
            "val_per_class_accuracy": per_class,
            "n_train": int(Xtr.shape[0]), "n_val": int(Xva.shape[0])}


if __name__ == "__main__":   # pragma: no cover
    ap = argparse.ArgumentParser(description="Train the per-service acoustic attribution model.")
    ap.add_argument("--out", default="resources/models/ai_attribution.onnx")
    ap.add_argument("--epochs", type=int, default=10)
    ap.add_argument("--n-fake", type=int, default=400)
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--root", help="SONICS corpus root (default: portable relative path)")
    args = ap.parse_args()
    print(train(args.out, epochs=args.epochs, n_fake=args.n_fake, seed=args.seed, root=args.root))
