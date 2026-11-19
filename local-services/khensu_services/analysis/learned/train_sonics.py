"""Train a binary AI-generated-music detector on real SONICS + MUSDB18 +
MTG-Jamendo audio.

Reuses the 7-channel forensic features (`features.py`) + the compact CNN
(`model.py`, n_classes=1) and produces an ONNX model + calibration sidecar with
**real held-out metrics** (accuracy, ECE, temperature, per-source breakdown).
Provenance `sonics_musdb_jamendo_real` (2026-07-17: MTG-Jamendo added
alongside MUSDB18 as a second, ~14x larger, genre-diverse real-negative
source - see sonics_corpus.py's docstring); stays `intervention_validated:false`
+ opt-in until the full expert-reviewed corpus contract
(`docs/evaluation/corpus-and-intervention-study.md`) is satisfied and reviewed.

    python -m khensu_services.analysis.learned.train_sonics \
        --out resources/models/ai_detector.onnx --epochs 8 --n-fake 200
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any

import numpy as np

from khensu_services.analysis.learned.features import extract_forensic_features, N_MELS, N_CHANNELS
from khensu_services.analysis.learned.inference import _window
from khensu_services.analysis.learned.training_provenance import (
    assert_run_paths_available, build_run_manifest, dataloader_worker_init, seed_everything,
    seeded_dataloader_generator, write_run_manifest, write_selected_data_manifest,
)

SR = 22050


def _ffmpeg_segment(path: str, offset: float, dur: float = 4.0) -> np.ndarray | None:
    """Decode a mono `dur`-second segment via ffmpeg (fast seek) → float32.

    librosa's mpg123 mp3 backend is ~100x slower than ffmpeg here (47 s/clip),
    so we shell ffmpeg with -ss before -i for near-instant seeking on both the
    SONICS mp3s and the MUSDB18 .stem.mp4 mixture stream."""
    import io
    import subprocess

    import soundfile as sf

    try:
        proc = subprocess.run(
            ["ffmpeg", "-nostdin", "-v", "quiet", "-ss", f"{offset:.2f}", "-t", f"{dur:.2f}",
             "-i", path, "-ar", str(SR), "-ac", "1", "-f", "wav", "pipe:1"],
            capture_output=True, timeout=60)
    except Exception:
        return None
    if proc.returncode != 0 or not proc.stdout:
        return None
    try:
        y, _ = sf.read(io.BytesIO(proc.stdout), dtype="float32")
    except Exception:
        return None
    return y if getattr(y, "ndim", 1) == 1 else y.mean(axis=1)


def _mp3_roundtrip(y: np.ndarray, sr: int, bitrate_k: int) -> np.ndarray | None:
    """Encode `y` to MP3 at `bitrate_k` kbps and decode it back, so a real
    negative carries the same MP3 codec artifacts as the SONICS positives
    (audit AI #1 - breaks the source/codec confound). Returns the decoded
    audio, or None if ffmpeg/libmp3lame is unavailable."""
    import io
    import subprocess

    import soundfile as sf

    buf = io.BytesIO()
    try:
        sf.write(buf, y.astype(np.float32), sr, format="WAV")
    except Exception:
        return None
    try:
        enc = subprocess.run(
            ["ffmpeg", "-nostdin", "-v", "quiet", "-f", "wav", "-i", "pipe:0",
             "-c:a", "libmp3lame", "-b:a", f"{bitrate_k}k", "-f", "mp3", "pipe:1"],
            input=buf.getvalue(), capture_output=True, timeout=60)
        if enc.returncode != 0 or not enc.stdout:
            return None
        dec = subprocess.run(
            ["ffmpeg", "-nostdin", "-v", "quiet", "-f", "mp3", "-i", "pipe:0",
             "-ar", str(sr), "-ac", "1", "-f", "wav", "pipe:1"],
            input=enc.stdout, capture_output=True, timeout=60)
        if dec.returncode != 0 or not dec.stdout:
            return None
        out, _ = sf.read(io.BytesIO(dec.stdout), dtype="float32")
    except Exception:
        return None
    out = out if getattr(out, "ndim", 1) == 1 else out.mean(axis=1)
    return np.asarray(out, dtype=np.float32)


def _featurize(clip) -> np.ndarray | None:
    y = _ffmpeg_segment(clip.path, clip.offset, 4.0)
    if y is None or y.size < SR:
        return None
    k = getattr(clip, "codec_match_mp3_k", 0)
    if k:
        rt = _mp3_roundtrip(np.asarray(y, dtype=np.float32), SR, int(k))
        if rt is not None and rt.size >= SR:
            y = rt[: y.size] if rt.size >= y.size else rt
    return _window(extract_forensic_features(np.asarray(y, dtype=np.float32), SR)).astype(np.float32)


def _load_split(clips):
    from khensu_services.analysis.learned.source_balance import source_of
    X, Y, S = [], [], []
    for c in clips:
        f = _featurize(c)
        if f is not None:
            X.append(f)
            Y.append(float(c.label))
            S.append(source_of(c))
    return np.stack(X), np.array(Y, dtype=np.float32), S


def _fit_temperature(logits, labels):
    if logits.size == 0:
        return 1.0
    best_t, best = 1.0, float("inf")
    for t in np.linspace(0.5, 4.0, 36):
        p = np.clip(1.0 / (1.0 + np.exp(-logits / t)), 1e-6, 1 - 1e-6)
        loss = float(-np.mean(labels * np.log(p) + (1 - labels) * np.log(1 - p)))
        if loss < best:
            best, best_t = loss, float(t)
    return best_t


def _ece(probs, labels, n_bins=10):
    edges = np.linspace(0, 1, n_bins + 1)
    e = 0.0
    for i in range(n_bins):
        m = (probs > edges[i]) & (probs <= edges[i + 1]) if i else (probs >= 0) & (probs <= edges[1])
        if m.any():
            e += m.mean() * abs(labels[m].mean() - probs[m].mean())
    return float(e)


def train(out_path: str, epochs: int = 8, n_fake: int = 200, real_tracks: int = 60,
          n_jamendo_tracks: int = 300, lr: float = 1e-3, batch_size: int = 16,
          codec_match: bool = False, seed: int = 0, root: str | None = None,
          jamendo_root: str | None = None) -> dict[str, Any]:
    import torch
    from torch.utils.data import DataLoader, TensorDataset

    from khensu_services.analysis.learned.model import build_cnn
    from khensu_services.analysis.learned.sonics_corpus import _JAMENDO_ROOT, _ROOT, build_corpus

    from khensu_services.analysis.learned.source_balance import (
        balance_by_source, per_source_metrics)
    from khensu_services.analysis.learned.model_card import build_model_card, write_model_card

    assert_run_paths_available(out_path)
    seeds = seed_everything(seed)
    train_clips, val_clips = build_corpus(root=root or _ROOT, n_fake=n_fake, real_tracks=real_tracks,
                                          n_jamendo_tracks=n_jamendo_tracks,
                                          codec_match=codec_match, seed=seed,
                                          jamendo_root=jamendo_root or _JAMENDO_ROOT)
    # Balance the training set across sources so no source/codec becomes a
    # shortcut feature (the narrow-negatives fix; no-op when clips are untagged).
    train_clips = balance_by_source(list(train_clips), seed=seed) or list(train_clips)
    Xtr, Ytr, _Str = _load_split(train_clips)
    Xva, Yva, Sva = _load_split(val_clips)
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
    # Per-source metrics WITH reliability-diagram bins (audit AI-detection #2):
    # persists the plottable calibration curve per source alongside the scalar
    # ECE so the UI/reviewer can see where the model is over/under-confident.
    per_source = per_source_metrics(probs, Yva, Sva, include_reliability=True)

    out = Path(out_path)
    out.parent.mkdir(parents=True, exist_ok=True)
    dummy = torch.zeros((1, N_CHANNELS, N_MELS, Xtr.shape[3]), dtype=torch.float32)
    from khensu_services.analysis.learned.onnx_export import legacy_onnx_export

    legacy_onnx_export(model, dummy, str(out), input_names=["features"],
                       output_names=["ai_logit"], opset_version=17,
                       dynamic_axes={"features": {0: "batch"}})
    codec_note = (" Real negatives were MP3-round-tripped to match the SONICS "
                  "positives' codec path (source/codec confound control, audit "
                  "AI #1)." if codec_match else "")
    data_manifest = write_selected_data_manifest(out, [
        {"path": c.path, "group": c.group, "label": c.label, "offset": c.offset,
         "source": c.source, "codec_match_mp3_k": c.codec_match_mp3_k, "split": split}
        for split, clips in (("train", train_clips), ("validation", val_clips)) for c in clips
    ])
    run_manifest = build_run_manifest(
        output_model=out, model_name="ai_detector", seeds=seeds,
        data_manifest=data_manifest,
        inputs={"root": root or _ROOT, "jamendo_root": jamendo_root or _JAMENDO_ROOT,
                "epochs": epochs, "n_fake": n_fake, "real_tracks": real_tracks,
                "n_jamendo_tracks": n_jamendo_tracks, "lr": lr, "batch_size": batch_size,
                "codec_match": codec_match},
        preprocessing={"feature_schema": "forensic_features", "sample_rate": SR,
                       "n_channels": N_CHANNELS, "n_mels": N_MELS, "window": "center_crop_or_tile_256"},
    )
    run_manifest_name = write_run_manifest(out, run_manifest)
    out.with_suffix(".json").write_text(json.dumps({
        "provenance": "sonics_musdb_jamendo_real",
        "task": "binary_ai_generated_music",
        "intervention_validated": False,
        "codec_matched_negatives": bool(codec_match),
        "temperature": round(temperature, 3),
        "val_accuracy": round(acc, 3),
        "val_ece": round(ece, 3),
        "val_per_source": per_source,
        "n_train": int(Xtr.shape[0]), "n_val": int(Xva.shape[0]),
        "run_manifest": run_manifest_name,
        "data_manifest": data_manifest["filename"],
        "limitation": "Real negatives are MUSDB18 + an MTG-Jamendo sample (broader "
                      "than MUSDB18 alone, but still not the full expert-reviewed "
                      "corpus contract). May still reflect source/production to some "
                      "degree rather than a purely general AI-artifact signal — see "
                      "per-source metrics. Not validated — opt-in, confidence withheld."
                      + codec_note,
    }, indent=2), encoding="utf-8")

    card = build_model_card(
        name="ai_detector", task="binary_ai_generated_music",
        provenance="sonics_musdb_jamendo_real", val_accuracy=acc, val_ece=ece,
        temperature=temperature, n_train=int(Xtr.shape[0]), n_val=int(Xva.shape[0]),
        per_source_metrics=per_source, intervention_validated=False,
        intended_use="Opt-in forensic aid; not a standalone AI-vs-human verdict.",
        license_note="SONICS training data CC-BY-NC-4.0 (research/eval only); "
                     "MTG-Jamendo audio is Creative-Commons-sourced, Apache-2.0 "
                     "distribution — see corpus_data/README.md.",
        limitations=[
            "Real negatives are MUSDB18 (studio .mp4) + an MTG-Jamendo sample (opus) "
            "vs SONICS mp3 positives — broader than MUSDB18 alone, but the model may "
            "still exploit source/codec separability rather than a fully general "
            "AI-artifact signal. See per-source metrics.",
            "Confidence withheld until the full expert-reviewed corpus/intervention "
            "study (docs/evaluation/corpus-and-intervention-study.md) clears review.",
        ])
    write_model_card(str(out), card)

    return {"onnx": str(out), "val_accuracy": acc, "val_ece": ece,
            "temperature": temperature, "val_per_source": per_source,
            "n_train": int(Xtr.shape[0]), "n_val": int(Xva.shape[0])}


if __name__ == "__main__":   # pragma: no cover
    ap = argparse.ArgumentParser(description="Train the real-data binary AI-music detector.")
    ap.add_argument("--out", default="resources/models/ai_detector.onnx")
    ap.add_argument("--epochs", type=int, default=8)
    ap.add_argument("--n-fake", type=int, default=200)
    ap.add_argument("--real-tracks", type=int, default=60, help="MUSDB18 tracks")
    ap.add_argument("--n-jamendo-tracks", type=int, default=300, help="MTG-Jamendo tracks")
    ap.add_argument("--codec-match", action="store_true",
                    help="MP3-round-trip real negatives to match the SONICS codec "
                         "path (breaks the source/codec confound, audit AI #1)")
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--root", help="SONICS/MUSDB corpus root (default: portable relative path)")
    ap.add_argument("--jamendo-root", help="MTG-Jamendo extracted-audio directory")
    args = ap.parse_args()
    print(train(args.out, epochs=args.epochs, n_fake=args.n_fake, real_tracks=args.real_tracks,
               n_jamendo_tracks=args.n_jamendo_tracks, codec_match=args.codec_match,
               seed=args.seed, root=args.root, jamendo_root=args.jamendo_root))
