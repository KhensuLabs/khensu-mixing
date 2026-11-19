"""Synthetic bootstrap training for the learned leak-gate bleed classifier.

No MUSDB-style stem-leak corpus is available, so this synthesizes wanted signal
concentrated in some bands + low-level inter-stem "bleed" injected into other
bands, with per-band bleed labels, trains the per-frame MLP, and exports ONNX.
Functional end-to-end but NOT validated on real leak — the model stays behind
the ``KHENSU_ENABLE_LEAK_MODEL`` opt-in (2026-07-15 safety audit). Replace with
a MUSDB-trained model per the same interface.

    python -m khensu_services.copilot.leak_gate_bootstrap \
        --out resources/models/leak_gate.onnx --epochs 12
"""
from __future__ import annotations

import argparse
from pathlib import Path
from typing import Any

import numpy as np

from khensu_services.copilot.leak_gate_classifier import (
    BAND_EDGES, N_BANDS, N_FEAT, extract_features, build_model,
)

SR = 22050


def _band_noise(rng, n, lo, hi, amp):
    from scipy.signal import butter, sosfilt
    hi = min(hi, SR / 2.0 - 100.0)
    sos = butter(4, [max(20.0, lo), hi], btype="bandpass", fs=SR, output="sos")
    return amp * sosfilt(sos, rng.standard_normal(n)).astype(np.float32)


def _sample(rng, dur=1.5):
    """One clip: wanted bands (loud) + bleed bands (quiet) → (audio, band_labels)."""
    n = int(dur * SR)
    edges = BAND_EDGES + [SR / 2.0]
    labels = np.zeros(N_BANDS, dtype=np.float32)
    y = np.zeros(n, dtype=np.float32)

    wanted = rng.choice(N_BANDS, size=rng.integers(1, 3), replace=False)
    for b in wanted:
        y += _band_noise(rng, n, edges[b], edges[b + 1], amp=rng.uniform(0.4, 0.8))
    # Bleed into some non-wanted bands at low level.
    for b in range(N_BANDS):
        if b in wanted:
            continue
        if rng.random() < 0.6:
            y += _band_noise(rng, n, edges[b], edges[b + 1], amp=rng.uniform(0.02, 0.08))
            labels[b] = 1.0
    peak = float(np.max(np.abs(y)) + 1e-9)
    return (0.9 * y / peak).astype(np.float32), labels


def _dataset(rng, n_clips):
    X, Y = [], []
    for _ in range(n_clips):
        y, labels = _sample(rng)
        feats = extract_features(y, SR)                    # (frames, N_BANDS*N_FEAT)
        X.append(feats)
        Y.append(np.tile(labels, (feats.shape[0], 1)))     # per-frame, per-band
    return np.concatenate(X).astype(np.float32), np.concatenate(Y).astype(np.float32)


def train(out_path: str, epochs: int = 12, n_clips: int = 48, seed: int = 0) -> dict[str, Any]:
    import torch

    rng = np.random.default_rng(seed)
    Xtr, Ytr = _dataset(rng, n_clips)
    Xva, Yva = _dataset(rng, max(8, n_clips // 4))

    model = build_model()
    opt = torch.optim.Adam(model.parameters(), lr=1e-3)
    loss_fn = torch.nn.BCEWithLogitsLoss()
    xb, yb = torch.from_numpy(Xtr), torch.from_numpy(Ytr)
    for _ in range(epochs):
        model.train()
        opt.zero_grad()
        loss_fn(model(xb), yb).backward()
        opt.step()

    model.eval()
    with torch.no_grad():
        pred = torch.sigmoid(model(torch.from_numpy(Xva))).numpy()
    acc = float(((pred > 0.5) == (Yva > 0.5)).mean())

    out = Path(out_path)
    out.parent.mkdir(parents=True, exist_ok=True)
    dummy = torch.zeros((1, N_BANDS * N_FEAT), dtype=torch.float32)
    from khensu_services.analysis.learned.onnx_export import legacy_onnx_export

    legacy_onnx_export(model, dummy, str(out), input_names=["features"],
                       output_names=["bleed_logits"], opset_version=17,
                       dynamic_axes={"features": {0: "frames"}})
    import json
    out.with_suffix(".json").write_text(json.dumps({
        "provenance": "synthetic_bootstrap",
        "intervention_validated": False,
        "val_accuracy": round(acc, 3),
        "bands": ["sub", "low", "mid", "high", "air"],
        "note": "Trained on synthetic bleed — functional but not validated on real MUSDB leak.",
    }, indent=2), encoding="utf-8")
    return {"onnx": str(out), "val_accuracy": acc}


if __name__ == "__main__":   # pragma: no cover
    ap = argparse.ArgumentParser(description="Train the synthetic bootstrap leak-gate classifier.")
    ap.add_argument("--out", default="resources/models/leak_gate.onnx")
    ap.add_argument("--epochs", type=int, default=12)
    ap.add_argument("--n", type=int, default=48)
    args = ap.parse_args()
    print(train(args.out, epochs=args.epochs, n_clips=args.n))
