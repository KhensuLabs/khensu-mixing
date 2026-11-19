"""Train the leak-gate bleed classifier on REAL MUSDB18 stem bleed.

Replaces the synthetic bootstrap with real isolated stems: for a track, one stem
is the *wanted* signal and attenuated sibling stems are injected as *bleed*; the
per-band, per-frame label is "bleed dominates this band here" computed from the
separate stems (bleed_energy > wanted_energy). Features come from the summed
mix — exactly what the gate sees at runtime. Real signal on both sides; still
opt-in + `intervention_validated:false` until reviewed.

MUSDB18 ships `.stem.mp4` (stream 0=mixture, 1=drums, 2=bass, 3=other,
4=vocals); stems are pulled with ffmpeg stream-mapping (ffmpeg is required).

    python -m khensu_services.copilot.leak_gate_realdata \
        --out resources/models/leak_gate.onnx --epochs 12 --tracks 40
"""
from __future__ import annotations

import argparse
import glob
import io
import json
import os
import subprocess
from pathlib import Path
from typing import Any

import numpy as np

from khensu_services.copilot.leak_gate_classifier import (
    BAND_EDGES, N_BANDS, N_FEAT, extract_features, build_model, _HOP,
)

SR = 22050
_ROOT = r"D:/Khensu Mixing/Model Weights/musdb18/train"
_STEMS = {"drums": 1, "bass": 2, "other": 3, "vocals": 4}


def _extract_stem(path: str, stream: int, offset: float, dur: float = 6.0) -> np.ndarray | None:
    try:
        proc = subprocess.run(
            ["ffmpeg", "-nostdin", "-v", "quiet", "-ss", f"{offset:.2f}", "-t", f"{dur:.2f}",
             "-i", path, "-map", f"0:{stream}", "-ar", str(SR), "-ac", "1", "-f", "wav", "pipe:1"],
            capture_output=True, timeout=60)
    except Exception:
        return None
    if proc.returncode != 0 or not proc.stdout:
        return None
    try:
        import soundfile as sf
        y, _ = sf.read(io.BytesIO(proc.stdout), dtype="float32")
    except Exception:
        return None
    return y if getattr(y, "ndim", 1) == 1 else y.mean(axis=1)


def _band_energy(y: np.ndarray) -> np.ndarray:
    """(N_BANDS, n_frames) band energy, aligned to extract_features frames."""
    import librosa
    S = np.abs(librosa.stft(y, n_fft=2048, hop_length=_HOP)) ** 2 + 1e-9
    freqs = librosa.fft_frequencies(sr=SR, n_fft=2048)
    edges = BAND_EDGES + [SR / 2.0]
    return np.stack([S[(freqs >= edges[i]) & (freqs < edges[i + 1])].sum(axis=0)
                     for i in range(N_BANDS)], axis=0)


def _samples_from_track(path: str, rng: np.random.Generator, n_seg: int = 3):
    """Yield (features, per-band-per-frame labels) for a few segments."""
    import librosa
    try:
        dur = float(librosa.get_duration(path=path))
    except Exception:
        return
    for _ in range(n_seg):
        off = float(rng.uniform(10.0, max(11.0, dur - 8.0)))
        stems = {name: _extract_stem(path, s, off) for name, s in _STEMS.items()}
        stems = {k: v for k, v in stems.items() if v is not None and v.size >= SR}
        if len(stems) < 2:
            continue
        names = list(stems)
        wanted_name = names[int(rng.integers(len(names)))]
        n = min(len(v) for v in stems.values())
        wanted = stems[wanted_name][:n]
        bleed = np.zeros(n, dtype=np.float32)
        for name, v in stems.items():
            if name != wanted_name:
                bleed = bleed + rng.uniform(0.04, 0.12) * v[:n]
        mix = wanted + bleed
        we, be = _band_energy(wanted), _band_energy(bleed)
        m = min(we.shape[1], be.shape[1])
        labels = ((be[:, :m] > we[:, :m]) & (be[:, :m] > 0.02 * be.max())).astype(np.float32)  # (bands, frames)
        feats = extract_features(mix, SR)                    # (frames, bands*feat)
        mm = min(feats.shape[0], labels.shape[1])
        if mm < 8:
            continue
        yield feats[:mm], labels[:, :mm].T                   # (frames, bands)


def train(out_path: str, epochs: int = 12, tracks: int = 40, seed: int = 0) -> dict[str, Any]:
    import torch

    rng = np.random.default_rng(seed)
    files = sorted(glob.glob(os.path.join(_ROOT, "*.stem.mp4")))[:tracks]
    X, Y, groups = [], [], []
    for gi, f in enumerate(files):
        for feats, labels in _samples_from_track(f, rng):
            X.append(feats)
            Y.append(labels)
            groups.append([gi] * feats.shape[0])
    if not X:
        raise ValueError("no MUSDB training data extracted (ffmpeg/stems?)")
    Xall, Yall = np.concatenate(X).astype(np.float32), np.concatenate(Y).astype(np.float32)
    gall = np.concatenate(groups)
    val_groups = set(np.unique(gall)[::5].tolist())
    tr = ~np.isin(gall, list(val_groups))
    va = np.isin(gall, list(val_groups))

    model = build_model()
    opt = torch.optim.Adam(model.parameters(), lr=1e-3)
    loss_fn = torch.nn.BCEWithLogitsLoss()
    xb, yb = torch.from_numpy(Xall[tr]), torch.from_numpy(Yall[tr])
    for _ in range(epochs):
        model.train()
        opt.zero_grad()
        loss_fn(model(xb), yb).backward()
        opt.step()

    model.eval()
    with torch.no_grad():
        pv = torch.sigmoid(model(torch.from_numpy(Xall[va]))).numpy()
    acc = float(((pv > 0.5) == (Yall[va] > 0.5)).mean()) if va.any() else 0.0

    out = Path(out_path)
    out.parent.mkdir(parents=True, exist_ok=True)
    dummy = torch.zeros((1, N_BANDS * N_FEAT), dtype=torch.float32)
    from khensu_services.analysis.learned.onnx_export import legacy_onnx_export

    legacy_onnx_export(model, dummy, str(out), input_names=["features"],
                       output_names=["bleed_logits"], opset_version=17,
                       dynamic_axes={"features": {0: "frames"}})
    out.with_suffix(".json").write_text(json.dumps({
        "provenance": "musdb18_real", "intervention_validated": False,
        "val_accuracy": round(acc, 3), "n_frames": int(Xall.shape[0]),
        "bands": ["sub", "low", "mid", "high", "air"],
        "note": "Trained on real MUSDB18 stem bleed (wanted stem + attenuated "
                "siblings). Opt-in until reviewed.",
    }, indent=2), encoding="utf-8")
    return {"onnx": str(out), "val_accuracy": acc, "n_frames": int(Xall.shape[0])}


if __name__ == "__main__":   # pragma: no cover
    ap = argparse.ArgumentParser(description="Train the leak-gate on real MUSDB18 stems.")
    ap.add_argument("--out", default="resources/models/leak_gate.onnx")
    ap.add_argument("--epochs", type=int, default=12)
    ap.add_argument("--tracks", type=int, default=40)
    args = ap.parse_args()
    print(train(args.out, epochs=args.epochs, tracks=args.tracks))
