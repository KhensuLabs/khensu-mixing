"""Real-audio corpus for the learned AI-artifact model — replaces
``bootstrap_corpus.py``'s synthetic sine/noise clips + hand-synthesized
artifact signatures with real music (MUSDB18 + MTG-Jamendo, see
``corpus_data/README.md``) and real signal-processing artifact injectors:

  * rvq_ghosting       — a real encode/decode round trip through Meta's
                         EnCodec (an actual RVQ neural codec) at a low target
                         bandwidth. Genuine quantization-ghosting artifacts,
                         not a synthesized comb of tones.
  * codec_residue      — a real lossy transcode via ffmpeg (reuses
                         ``codec_artifacts.encode_and_decode``, the same
                         helper behind the "Codec / transcode artifact audit"
                         feature) at a low bitrate.
  * hf_aliasing        — a real decimate-without-anti-alias-filter
                         downsample + naive upsample. Genuine aliasing
                         folded from real harmonic content, not synthesized
                         bandpass noise.
  * hollow_compression — a real heavy-compressor + mid-scoop EQ chain via
                         pedalboard. Genuine dynamics-processing artifact on
                         real audio, not a static bandstop filter.

Each of the ~n_per_class * 6 clips this generates is sourced from a
*different* underlying real track (MUSDB18 train stems + MTG-Jamendo
extracted tracks, both already on disk), so there is no train/val leakage
risk from reusing one track across clips - unlike a corpus that segments a
handful of tracks many ways.

Still NOT a validated detector of real generative-AI audio: these are real
*production/transcode* artifacts - genuine instances of the same four
forensic signatures the model targets - not AI-generated source material
itself. `intervention_validated` stays false until the full expert-review
contract in docs/evaluation/corpus-and-intervention-study.md is satisfied;
this corpus is a large step up in data realism over the synthetic bootstrap,
not a substitute for that review.

    python -m khensu_services.analysis.learned.make_real_artifact_model \\
        --out resources/models/artifact_model.onnx --n 40
"""
from __future__ import annotations

import glob
import json
import os
import random
import subprocess
from pathlib import Path
from typing import Any

import numpy as np

from khensu_services.analysis.learned.model import ARTIFACT_CLASSES

SR = 22050
_MUSDB_ROOT = r"D:/Khensu Mixing/Model Weights/musdb18/train"
_JAMENDO_ROOT = r"D:/Khensu Mixing/corpus_data/mtg_jamendo_real/extracted"
_CLIP_DUR_SEC = 6.0


def _probe_duration_sec(path: str) -> float | None:
    try:
        proc = subprocess.run(
            ["ffprobe", "-v", "quiet", "-show_entries", "format=duration",
             "-of", "csv=p=0", path], capture_output=True, timeout=15, text=True)
        return float(proc.stdout.strip())
    except Exception:
        return None


def _real_source_clips(musdb_root: str, jamendo_root: str, n: int, dur: float,
                       seed: int) -> list[dict[str, Any]]:
    """Load up to n real mono clips (dur seconds each), one per underlying
    track, from MUSDB18 + MTG-Jamendo. Each clip carries a split_key (source
    track id) for manifest provenance (see DATA_SCHEMA.md)."""
    import librosa

    candidates: list[tuple[str, str]] = []
    if musdb_root and os.path.isdir(musdb_root):
        candidates += [("musdb", p) for p in
                       sorted(glob.glob(os.path.join(musdb_root, "*.stem.mp4")))]
    if jamendo_root and os.path.isdir(jamendo_root):
        candidates += [("jamendo", p) for p in
                       sorted(glob.glob(os.path.join(jamendo_root, "*.opus")))]
    if not candidates:
        raise FileNotFoundError(
            "no real source audio found under musdb_root/jamendo_root - see corpus_data/README.md")

    rng = random.Random(seed)
    rng.shuffle(candidates)

    clips: list[dict[str, Any]] = []
    for source, path in candidates:
        if len(clips) >= n:
            break
        dur_sec = _probe_duration_sec(path)
        if dur_sec is None or dur_sec < dur + 20.0:
            continue
        try:
            y, sr = librosa.load(path, sr=SR, mono=True, duration=dur, offset=15.0)
        except Exception:
            continue
        if len(y) < dur * SR * 0.5:
            continue
        clips.append({"y": y, "split_key": f"{source}_{os.path.basename(path)}", "source": source})
    return clips


def _add_rvq_real(y: np.ndarray) -> np.ndarray:
    """Real RVQ-codec ghosting via Meta's EnCodec (24kHz model, 6kbps target
    bandwidth) - an actual neural-codec quantization round trip, the real
    mechanism rvq_ghosting targets."""
    import torch
    from encodec import EncodecModel

    model = EncodecModel.encodec_model_24khz()
    model.set_target_bandwidth(6.0)
    model.eval()

    wav = torch.from_numpy(np.asarray(y, dtype=np.float32)).reshape(1, 1, -1)
    if SR != model.sample_rate:
        import torchaudio
        wav = torchaudio.functional.resample(wav, SR, model.sample_rate)
    with torch.no_grad():
        encoded = model.encode(wav)
        decoded = model.decode(encoded)
    out = decoded.reshape(-1).numpy()
    if model.sample_rate != SR:
        import librosa
        out = librosa.resample(out, orig_sr=model.sample_rate, target_sr=SR)
    n = min(len(out), len(y))
    return np.asarray(out[:n], dtype=np.float32)


def _add_codec_residue_real(y: np.ndarray, tmp_dir: str) -> np.ndarray:
    """Real low-bitrate MP3 transcode round trip via the same ffmpeg helper
    behind the codec/transcode artifact audit feature - a genuine lossy
    encode's residue, not synthesized amplitude modulation."""
    import soundfile as sf

    from khensu_services.analysis.codec_artifacts import encode_and_decode

    src = os.path.join(tmp_dir, f"src_{random.randint(0, 1_000_000_000)}.wav")
    sf.write(src, y, SR)
    decoded_path = encode_and_decode(src, "libmp3lame", 64, "mp3", SR, tmp_dir)
    out, _ = sf.read(decoded_path, always_2d=False)
    out = np.asarray(out, dtype=np.float32)
    n = min(len(out), len(y))
    return out[:n]


def _add_hf_aliasing_real(y: np.ndarray, rng: np.random.Generator) -> np.ndarray:
    """Real decimate-without-anti-alias-filter downsample + naive linear
    upsample - genuine aliasing folded from real harmonic content (not
    synthesized bandpass noise), the classic artifact of a corner-cutting
    resampler."""
    factor = int(rng.integers(3, 6))
    decimated = y[::factor]
    if len(decimated) < 2:
        return y
    idx = np.linspace(0, len(decimated) - 1, len(y))
    return np.interp(idx, np.arange(len(decimated)), decimated).astype(np.float32)


def _add_hollow_real(y: np.ndarray) -> np.ndarray:
    """Real heavy compression + mid-scoop EQ via pedalboard - genuine
    dynamics-processing artifact on real audio, not a static bandstop
    filter."""
    from pedalboard import Compressor, Gain, Pedalboard, PeakFilter

    board = Pedalboard([
        Compressor(threshold_db=-28.0, ratio=10.0, attack_ms=1.0, release_ms=80.0),
        PeakFilter(cutoff_frequency_hz=800.0, gain_db=-14.0, q=0.7),
        Gain(gain_db=6.0),
    ])
    return np.asarray(board(y.reshape(1, -1), SR)[0], dtype=np.float32)


def _inject(cls: str, y: np.ndarray, rng: np.random.Generator, tmp_dir: str) -> np.ndarray:
    if cls == "rvq_ghosting":
        return _add_rvq_real(y)
    if cls == "codec_residue":
        return _add_codec_residue_real(y, tmp_dir)
    if cls == "hf_aliasing":
        return _add_hf_aliasing_real(y, rng)
    if cls == "hollow_compression":
        return _add_hollow_real(y)
    raise ValueError(f"unknown artifact class: {cls}")


def generate(out_dir: str, n_per_class: int = 40, dur: float = _CLIP_DUR_SEC, seed: int = 0,
            musdb_root: str = _MUSDB_ROOT, jamendo_root: str = _JAMENDO_ROOT) -> str:
    """Write real-audio clips + a JSONL manifest (path/labels/source/split_key
    per DATA_SCHEMA.md); return the manifest path. Needs
    n_per_class * (1 clean + len(ARTIFACT_CLASSES) + 1 multi-label) distinct
    real tracks - raises if MUSDB18/MTG-Jamendo don't have enough on disk."""
    import soundfile as sf

    total_needed = n_per_class * (1 + len(ARTIFACT_CLASSES) + 1)
    clips = _real_source_clips(musdb_root, jamendo_root, total_needed, dur, seed)
    if len(clips) < total_needed:
        raise ValueError(
            f"only found {len(clips)} usable real clips, need {total_needed} - "
            "fetch more MUSDB18/MTG-Jamendo audio (see corpus_data/README.md)")

    rng = np.random.default_rng(seed)
    out = Path(out_dir)
    out.mkdir(parents=True, exist_ok=True)
    rows: list[dict[str, Any]] = []
    idx = 0
    cursor = 0

    def _next_clip() -> dict[str, Any]:
        nonlocal cursor
        c = clips[cursor]
        cursor += 1
        return c

    def _write(y: np.ndarray, labels: dict[str, int], split_key: str, source: str) -> None:
        nonlocal idx
        y = np.asarray(y, dtype=np.float32)
        peak = float(np.max(np.abs(y)) + 1e-9)
        y = (0.9 * y / peak).astype(np.float32)
        p = out / f"clip_{idx:04d}.wav"
        sf.write(str(p), y, SR)
        rows.append({"path": p.name, "labels": labels, "source": source, "split_key": split_key})
        idx += 1

    # Clean negatives - untouched real audio.
    for _ in range(n_per_class):
        c = _next_clip()
        _write(c["y"], {cls: 0 for cls in ARTIFACT_CLASSES}, c["split_key"], "real_clean")

    # One-hot positives per class.
    for cls in ARTIFACT_CLASSES:
        for _ in range(n_per_class):
            c = _next_clip()
            y = _inject(cls, c["y"], rng, out_dir)
            _write(y, {cc: int(cc == cls) for cc in ARTIFACT_CLASSES}, c["split_key"], f"real_{cls}")

    # Multi-label co-occurrence.
    for _ in range(n_per_class):
        c = _next_clip()
        chosen = list(rng.choice(ARTIFACT_CLASSES, size=2, replace=False))
        y = c["y"]
        for cls in chosen:
            y = _inject(cls, y, rng, out_dir)
        _write(y, {cc: int(cc in chosen) for cc in ARTIFACT_CLASSES}, c["split_key"],
              "real_" + "+".join(sorted(chosen)))

    manifest = out / "manifest.jsonl"
    manifest.write_text("\n".join(json.dumps(r) for r in rows), encoding="utf-8")
    return str(manifest)


if __name__ == "__main__":   # pragma: no cover
    import argparse

    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out-dir", default="../corpus_data/artifact_corpus_real")
    ap.add_argument("--n-per-class", type=int, default=40)
    args = ap.parse_args()
    result = generate(args.out_dir, n_per_class=args.n_per_class)
    print(json.dumps({"manifest": result}, indent=2))
