"""Synthetic bootstrap corpus for the learned AI-artifact model.

No licensed AI-vs-human corpus exists yet (the documented P2.1 blocker), so this
generates a **synthetic** labeled corpus that embodies the four artifact
signatures the model targets — enough to train, export, and wire a real ONNX
model end-to-end and prove the pipeline, but explicitly NOT a validated detector
of real generative audio. A model trained here keeps `intervention_validated:
false` and must not be trusted as a reliability judgement (2026-07-15 safety
audit). Consistent with the project's existing "synthetic bootstrap benchmark
data" precedent.

Each clip is clean program material (mixed sines + light noise) optionally
injected with one or more artifact signatures:

  * rvq_ghosting      — a tight comb of narrowband tones above 10 kHz
  * codec_residue     — amplitude modulation of the 6-12 kHz band on sustains
  * hf_aliasing       — broadband 8-12 kHz grain/noise
  * hollow_compression— a scooped 300-2000 Hz mid band
"""
from __future__ import annotations

import json
from pathlib import Path

import numpy as np

from khensu_services.analysis.learned.model import ARTIFACT_CLASSES

SR = 22050


def _program(rng: np.random.Generator, dur: float) -> np.ndarray:
    """A clean-ish musical bed: a few harmonic tones + light broadband noise."""
    t = np.arange(int(dur * SR)) / SR
    f0 = float(rng.uniform(90, 320))
    y = np.zeros_like(t)
    for k in (1, 2, 3, 4):
        y += (0.3 / k) * np.sin(2 * np.pi * f0 * k * t + rng.uniform(0, 6.28))
    y += 0.02 * rng.standard_normal(t.shape)
    return y


def _add_rvq(y, rng):
    t = np.arange(len(y)) / SR
    base = float(rng.uniform(10200, 11000))
    for i in range(3):
        y = y + 0.18 * np.sin(2 * np.pi * (base + i * 25) * t)
    return y


def _add_codec_residue(y, rng):
    t = np.arange(len(y)) / SR
    carrier = float(rng.uniform(6500, 11000))
    mod = 0.5 + 0.5 * np.sin(2 * np.pi * float(rng.uniform(3, 9)) * t)
    return y + 0.12 * mod * np.sin(2 * np.pi * carrier * t)


def _add_hf_aliasing(y, rng):
    from scipy.signal import butter, sosfilt
    noise = rng.standard_normal(len(y))
    sos = butter(6, [8000, 11000], btype="bandpass", fs=SR, output="sos")
    return y + 0.15 * sosfilt(sos, noise)


def _add_hollow(y):
    from scipy.signal import butter, sosfilt
    sos = butter(4, [300, 2000], btype="bandstop", fs=SR, output="sos")
    return sosfilt(sos, y)


_INJECTORS = {
    "rvq_ghosting": lambda y, rng: _add_rvq(y, rng),
    "codec_residue": lambda y, rng: _add_codec_residue(y, rng),
    "hf_aliasing": lambda y, rng: _add_hf_aliasing(y, rng),
    "hollow_compression": lambda y, rng: _add_hollow(y),
}


def generate(out_dir: str, n_per_class: int = 12, dur: float = 2.0, seed: int = 0) -> str:
    """Write synthetic clips + a JSONL manifest; return the manifest path."""
    import soundfile as sf

    rng = np.random.default_rng(seed)
    out = Path(out_dir)
    out.mkdir(parents=True, exist_ok=True)
    rows: list[dict] = []
    idx = 0

    def _write(y, labels):
        nonlocal idx
        y = np.asarray(y, dtype=np.float32)
        peak = float(np.max(np.abs(y)) + 1e-9)
        y = (0.9 * y / peak).astype(np.float32)
        p = out / f"clip_{idx:04d}.wav"
        sf.write(str(p), y, SR)
        rows.append({"path": p.name, "labels": labels})
        idx += 1

    # Clean negatives.
    for _ in range(n_per_class):
        _write(_program(rng, dur), {c: 0 for c in ARTIFACT_CLASSES})

    # One-hot positives per class.
    for cls in ARTIFACT_CLASSES:
        for _ in range(n_per_class):
            y = _INJECTORS[cls](_program(rng, dur), rng)
            _write(y, {c: int(c == cls) for c in ARTIFACT_CLASSES})

    # A few multi-label positives (co-occurring artifacts).
    for _ in range(n_per_class):
        y = _program(rng, dur)
        chosen = rng.choice(ARTIFACT_CLASSES, size=2, replace=False)
        for cls in chosen:
            y = _INJECTORS[cls](y, rng)
        _write(y, {c: int(c in chosen) for c in ARTIFACT_CLASSES})

    manifest = out / "manifest.jsonl"
    manifest.write_text("\n".join(json.dumps(r) for r in rows), encoding="utf-8")
    return str(manifest)
