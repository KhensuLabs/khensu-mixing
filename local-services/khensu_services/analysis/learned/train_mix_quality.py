"""Self-supervised mix-quality model (pipeline #9).

Trains a binary classifier P(professional-quality) on the forensic feature
tensor: professionally-produced tracks (MUSDB18 + MTG-Jamendo, the same real
sources sonics_corpus uses) are the CLEAN class (label 1); the SAME tracks with
a programmatic mix defect applied (see degradations.py) are the DEFECTIVE class
(label 0). No human ratings needed — the model detects the presence of known,
fixable defects as an explainable proxy for quality.

Same discipline as train_sonics.py: leakage-safe split grouped by source track,
temperature-scaled logits, reliability diagram + ECE, model card with
`intervention_validated:false` (opt-in, confidence withheld until the
corpus/intervention study clears review). The retrain is opt-in and the corpus
data is git-ignored; nothing ships a model.
"""
from __future__ import annotations

import argparse
import json
import random
from pathlib import Path
from typing import Any

import numpy as np

from khensu_services.analysis.learned.degradations import DEFECT_KINDS, apply_defect
from khensu_services.analysis.learned.features import N_CHANNELS, N_MELS, extract_forensic_features
from khensu_services.analysis.learned.inference import _window
from khensu_services.analysis.learned.train_sonics import SR, _ece, _ffmpeg_segment, _fit_temperature
from khensu_services.analysis.learned.training_provenance import (
    assert_run_paths_available, build_run_manifest, dataloader_worker_init, seed_everything,
    seeded_dataloader_generator, write_run_manifest, write_selected_data_manifest,
)


def _real_clips(root: str | None, jamendo_root: str | None, n_real_seg: int,
                musdb_tracks: int, jamendo_tracks: int, seed: int = 0):
    from khensu_services.analysis.learned.sonics_corpus import (
        _ROOT, _JAMENDO_ROOT, _jamendo_clips, _musdb_clips)
    import os
    clips = _musdb_clips(root or _ROOT, n_real_seg, musdb_tracks)
    jr = jamendo_root or _JAMENDO_ROOT
    if jr and os.path.isdir(jr):
        clips += _jamendo_clips(jr, n_real_seg, jamendo_tracks, seed=seed + 1)
    return clips


def _featurize(y: np.ndarray) -> np.ndarray | None:
    if y is None or y.size < SR:
        return None
    return _window(extract_forensic_features(np.asarray(y, dtype=np.float32), SR)).astype(np.float32)


def _build_split(clips, val_frac: float, seed: int):
    """Each real clip yields a CLEAN example (label 1) and a DEFECTIVE example
    (label 0, one randomized defect). Split grouped by source track so a track's
    clean and degraded halves never straddle train/val."""
    groups = sorted({c.group for c in clips})
    rng = random.Random(seed)
    rng.shuffle(groups)
    n_val = max(1, int(len(groups) * val_frac))
    val_groups = set(groups[:n_val])

    drng = np.random.default_rng(seed)
    Xtr, Ytr, Xva, Yva = [], [], [], []
    for c in clips:
        y = _ffmpeg_segment(c.path, c.offset, 4.0)
        if y is None:
            continue
        clean = _featurize(y)
        kind = str(drng.choice(DEFECT_KINDS))
        bad = _featurize(apply_defect(y, SR, kind, drng))
        if clean is None or bad is None:
            continue
        into_val = c.group in val_groups
        (Xva if into_val else Xtr).extend([clean, bad])
        (Yva if into_val else Ytr).extend([1.0, 0.0])
    return (np.stack(Xtr), np.array(Ytr, dtype=np.float32),
            np.stack(Xva), np.array(Yva, dtype=np.float32))


def train(out_path: str, epochs: int = 8, musdb_tracks: int = 60, jamendo_tracks: int = 200,
          n_real_seg: int = 4, lr: float = 1e-3, batch_size: int = 16,
          val_frac: float = 0.2, seed: int = 0, root: str | None = None,
          jamendo_root: str | None = None) -> dict[str, Any]:
    import torch
    from torch.utils.data import DataLoader, TensorDataset

    from khensu_services.analysis.learned.model import build_cnn
    from khensu_services.analysis.learned.model_card import build_model_card, write_model_card
    from khensu_services.analysis.learned.source_balance import reliability_diagram

    assert_run_paths_available(out_path)
    seeds = seed_everything(seed)
    clips = _real_clips(root, jamendo_root, n_real_seg, musdb_tracks, jamendo_tracks, seed=seed)
    Xtr, Ytr, Xva, Yva = _build_split(clips, val_frac, seed)
    if Xtr.shape[0] < 8 or Xva.shape[0] < 4:
        raise ValueError(f"corpus too small (train {Xtr.shape[0]}, val {Xva.shape[0]})")

    model = build_cnn(in_channels=N_CHANNELS, n_classes=1, base=16)
    opt = torch.optim.Adam(model.parameters(), lr=lr)
    loss_fn = torch.nn.BCEWithLogitsLoss()
    loader = DataLoader(TensorDataset(torch.from_numpy(Xtr), torch.from_numpy(Ytr)),
                        batch_size=batch_size, shuffle=True,
                        generator=seeded_dataloader_generator(seed),
                        worker_init_fn=dataloader_worker_init(seed))
    for _ in range(epochs):
        model.train()
        for xb, yb in loader:
            opt.zero_grad()
            loss_fn(model(xb).squeeze(-1), yb).backward()
            opt.step()

    model.eval()
    with torch.no_grad():
        va_logits = model(torch.from_numpy(Xva)).squeeze(-1).numpy()
    temperature = _fit_temperature(va_logits, Yva)
    probs = 1.0 / (1.0 + np.exp(-va_logits / temperature))
    acc = float(((probs > 0.5) == (Yva > 0.5)).mean())
    ece = _ece(probs, Yva)
    reliability = reliability_diagram(probs, Yva)

    out = Path(out_path)
    out.parent.mkdir(parents=True, exist_ok=True)
    dummy = torch.zeros((1, N_CHANNELS, N_MELS, Xtr.shape[3]), dtype=torch.float32)
    from khensu_services.analysis.learned.onnx_export import legacy_onnx_export

    legacy_onnx_export(model, dummy, str(out), input_names=["features"],
                       output_names=["quality_logit"], opset_version=17,
                       dynamic_axes={"features": {0: "batch"}})
    manifest_groups = sorted({c.group for c in clips})
    manifest_rng = random.Random(seed)
    manifest_rng.shuffle(manifest_groups)
    validation_groups = set(manifest_groups[:max(1, int(len(manifest_groups) * val_frac))])
    data_manifest = write_selected_data_manifest(out, [
        {"path": c.path, "group": c.group, "offset": c.offset, "source": c.source,
         "split": "validation" if c.group in validation_groups else "train"}
        for c in clips
    ])
    run_manifest = build_run_manifest(
        output_model=out, model_name="mix_quality", seeds=seeds,
        data_manifest=data_manifest,
        inputs={"root": root, "jamendo_root": jamendo_root, "epochs": epochs,
                "musdb_tracks": musdb_tracks, "jamendo_tracks": jamendo_tracks,
                "n_real_seg": n_real_seg, "lr": lr, "batch_size": batch_size,
                "val_frac": val_frac},
        preprocessing={"feature_schema": "forensic_features", "sample_rate": SR,
                       "n_channels": N_CHANNELS, "n_mels": N_MELS,
                       "window": "center_crop_or_tile_256", "defect_kinds": list(DEFECT_KINDS)},
    )
    run_manifest_name = write_run_manifest(out, run_manifest)
    out.with_suffix(".json").write_text(json.dumps({
        "provenance": "self_supervised_degradation",
        "task": "mix_quality_defect_free",
        "intervention_validated": False,
        "defect_kinds": list(DEFECT_KINDS),
        "temperature": round(temperature, 3),
        "val_accuracy": round(acc, 3),
        "val_ece": round(ece, 3),
        "reliability_diagram": reliability,
        "n_train": int(Xtr.shape[0]), "n_val": int(Xva.shape[0]),
        "run_manifest": run_manifest_name,
        "data_manifest": data_manifest["filename"],
        "limitation": "Self-supervised: detects the presence of KNOWN programmatic "
                      "defects (clipping/harshness/mud/overcompression) applied to real "
                      "tracks, an explainable proxy for quality - NOT a subjective "
                      "human quality rating and not exhaustive of real-world defects. "
                      "Confidence withheld (opt-in) until the corpus/intervention study.",
    }, indent=2), encoding="utf-8")

    card = build_model_card(
        name="mix_quality", task="mix_quality_defect_free",
        provenance="self_supervised_degradation", val_accuracy=acc, val_ece=ece,
        temperature=temperature, n_train=int(Xtr.shape[0]), n_val=int(Xva.shape[0]),
        intervention_validated=False,
        intended_use="Opt-in forensic aid: flags likely mix defects vs a clean "
                     "reference. Not a standalone quality verdict.",
        license_note="Trained on MUSDB18 + MTG-Jamendo real audio (see corpus_data/README.md).",
        limitations=[
            "Self-supervised proxy: learns the 4 programmatic defect classes, so a "
            "defect it was never shown (or a genuinely novel production style) may "
            "read as clean or defective incorrectly.",
            "Confidence withheld until the corpus/intervention study clears review.",
        ])
    write_model_card(str(out), card)

    return {"onnx": str(out), "val_accuracy": acc, "val_ece": ece,
            "temperature": temperature, "n_train": int(Xtr.shape[0]), "n_val": int(Xva.shape[0])}


if __name__ == "__main__":   # pragma: no cover
    ap = argparse.ArgumentParser(description="Train the self-supervised mix-quality model.")
    ap.add_argument("--out", default="resources/models/mix_quality.onnx")
    ap.add_argument("--epochs", type=int, default=8)
    ap.add_argument("--musdb-tracks", type=int, default=60)
    ap.add_argument("--jamendo-tracks", type=int, default=200)
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--root", help="MUSDB/SONICS corpus root (default: portable relative path)")
    ap.add_argument("--jamendo-root", help="MTG-Jamendo extracted-audio directory")
    args = ap.parse_args()
    print(train(args.out, epochs=args.epochs, musdb_tracks=args.musdb_tracks,
                jamendo_tracks=args.jamendo_tracks, seed=args.seed,
                root=args.root, jamendo_root=args.jamendo_root))
