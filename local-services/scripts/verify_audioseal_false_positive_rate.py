"""Verify AudioSeal's false-positive rate on real (non-AI-generated) music.

`audioseal_tier.py`'s AudioSealWatermarkDetector flags a clip when the
detector's mean probability >= 0.5. That threshold has never been checked
against a real, diverse music sample - this script does that check against
corpus_data/mtg_jamendo_real/extracted/ (Creative-Commons-sourced human
music; see corpus_data/README.md), which isn't present in CI.

    python scripts/verify_audioseal_false_positive_rate.py --n 300

2026-07-17 result (n=300, seed=0, 10s window per track): mean prob 0.019,
max 0.361, std 0.044, false-positive rate at the production 0.5 threshold =
0/300 (0.0%). At a hypothetically much more sensitive 0.3 threshold: 2/300
(0.67%). The shipped 0.5 threshold has zero observed false positives on this
sample.
"""
from __future__ import annotations

import argparse
import glob
import random
import time

import numpy as np


def run(n: int, seed: int, jamendo_dir: str) -> dict:
    import librosa
    import torch
    from audioseal import AudioSeal

    det = AudioSeal.load_detector("audioseal_detector_16bits")

    files = sorted(glob.glob(f"{jamendo_dir}/*.opus"))
    if not files:
        raise SystemExit(f"No .opus files found under {jamendo_dir} - see corpus_data/README.md")
    random.Random(seed).shuffle(files)
    files = files[:n]

    probs: list[float] = []
    errors = 0
    t0 = time.time()
    for f in files:
        try:
            y, sr = librosa.load(f, sr=16000, mono=True, duration=10.0, offset=10.0)
            if len(y) < sr:
                y, sr = librosa.load(f, sr=16000, mono=True, duration=10.0)
            if len(y) < sr:
                errors += 1
                continue
            wav = torch.from_numpy(y.astype(np.float32)).unsqueeze(0).unsqueeze(0)
            with torch.no_grad():
                prob_tensor, _ = det.detect_watermark(wav, sr)
            probs.append(float(prob_tensor.mean().item()))
        except Exception:
            errors += 1

    arr = np.asarray(probs, dtype=np.float64)
    elapsed = time.time() - t0
    return {
        "n": int(arr.size), "errors": errors, "elapsed_sec": round(elapsed, 1),
        "mean_prob": round(float(arr.mean()), 4) if arr.size else None,
        "max_prob": round(float(arr.max()), 4) if arr.size else None,
        "std_prob": round(float(arr.std()), 4) if arr.size else None,
        "false_positive_rate": {
            str(t): round(float((arr >= t).mean()), 4) if arr.size else None
            for t in (0.5, 0.3, 0.1)
        },
    }


if __name__ == "__main__":   # pragma: no cover
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--n", type=int, default=300)
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--jamendo-dir", default="../corpus_data/mtg_jamendo_real/extracted")
    args = ap.parse_args()
    print(run(args.n, args.seed, args.jamendo_dir))
