"""Real-data corpus for a binary AI-generated-music detector.

Uses the downloaded SONICS + MUSDB18 data (under ``Model Weights/``), plus an
MTG-Jamendo real-music sample (under ``corpus_data/``, see
``corpus_data/README.md``):
  * **AI positives** — SONICS fake songs (`fake_songs/**/fake_*_<algo>_*.mp3`),
    real generations from Suno/Udio/etc.
  * **Real negatives** — MUSDB18 mixtures (`musdb18/train/*.stem.mp4`, the
    default/mixture stream via librosa+ffmpeg) AND MTG-Jamendo tracks
    (`corpus_data/mtg_jamendo_real/extracted/*.opus`, Creative-Commons-sourced,
    genre/instrument/mood-tagged, ~2100 tracks — see ``jamendo_root`` below),
    both professionally-produced/real human music. Segmented so a handful of
    tracks yields many negatives.

This is genuine real audio on both sides (a big step past the synthetic
bootstrap). MUSDB18 alone was a **narrow** negative set (150 tracks, mostly
indie/rock/pop test-set stems), which risked a model learning "MUSDB-like vs
SONICS-like" rather than a true artifact detector; MTG-Jamendo adds ~14x more
tracks across a much wider genre spread as a second, independent real source
(2026-07-17). It still isn't the full expert-reviewed, leak-safe corpus
contract in `docs/evaluation/corpus-and-intervention-study.md`, so the model
stays `intervention_validated:false` and opt-in, with the limitation recorded
in the model card. Splits are grouped by source track id to avoid segment
leakage across train/val.
"""
from __future__ import annotations

import glob
import os
import random
import subprocess
from dataclasses import dataclass

# Portable, explicit defaults relative to the service working directory. Callers
# may pass a root directly or configure these in their training environment.
_ROOT = os.environ.get("KHENSU_SONICS_ROOT", "../Model Weights")
_JAMENDO_ROOT = os.environ.get(
    "KHENSU_JAMENDO_ROOT", "../corpus_data/mtg_jamendo_real/extracted")
SEG_SEC = 4.0


@dataclass
class Clip:
    path: str
    label: int          # 1 = AI, 0 = real
    group: str          # source-track id (leakage-safe split key)
    offset: float = 0.0
    # Source tag consumed by source_balance.py's source_of() for balanced
    # sampling + per-source metrics. Previously unset on every clip this
    # module produced, so that balancing/reporting silently lumped
    # everything under "unknown" (fixed 2026-07-17 while adding the Jamendo
    # source, since a second real source is exactly when per-source
    # visibility starts to matter).
    source: str = "unknown"
    # kbps to re-encode this clip through (MP3 round-trip) before featurizing,
    # or 0 for no round-trip. Breaks the source/codec confound (audit AI #1):
    # SONICS positives are MP3, but MUSDB18 (.mp4/AAC) and Jamendo (.opus)
    # negatives are not, so a detector can shortcut on "MP3 encoder artifacts
    # = AI". Re-encoding the *real* negatives through the SAME MP3 path puts
    # codec artifacts on both sides, forcing the model onto the actual
    # generation artifact. Set on negatives by build_corpus(codec_match=...).
    codec_match_mp3_k: int = 0


def _fake_clips(root: str, limit: int, seed: int = 0) -> list[Clip]:
    files = glob.glob(os.path.join(root, "fake_songs", "**", "fake_*.mp3"), recursive=True)
    files.sort()
    random.Random(seed).shuffle(files)
    clips = []
    for f in files[:limit]:
        # fake_00001_suno_0.mp3 -> group "fake_00001" (all takes of one song),
        # source "sonics_suno" (the generator, for per-source metrics).
        base = os.path.basename(f)
        parts = base.split("_")
        group = "_".join(parts[:2])
        algo = parts[2] if len(parts) > 2 else "unknown"
        clips.append(Clip(path=f, label=1, group=group, offset=20.0, source=f"sonics_{algo}"))
    return clips


def _musdb_clips(root: str, n_segments: int, limit_tracks: int) -> list[Clip]:
    import librosa

    tracks = sorted(glob.glob(os.path.join(root, "musdb18", "train", "*.stem.mp4")))[:limit_tracks]
    clips = []
    for t in tracks:
        try:
            dur = float(librosa.get_duration(path=t))
        except Exception:
            continue
        usable = max(0.0, dur - SEG_SEC - 10.0)
        if usable <= 0:
            continue
        step = usable / max(1, n_segments)
        for i in range(n_segments):
            clips.append(Clip(path=t, label=0, group="musdb_" + os.path.basename(t),
                              offset=10.0 + i * step, source="musdb18"))
    return clips


def _probe_duration_sec(path: str) -> float | None:
    """Duration via ffprobe rather than librosa - MTG-Jamendo ships .opus,
    and ffprobe/ffmpeg (already required for the .stem.mp4/.mp3 decode path
    below) handles it without depending on libsndfile's opus support."""
    try:
        proc = subprocess.run(
            ["ffprobe", "-v", "quiet", "-show_entries", "format=duration",
             "-of", "csv=p=0", path],
            capture_output=True, timeout=15, text=True)
        return float(proc.stdout.strip())
    except Exception:
        return None


def _jamendo_clips(root: str, n_segments: int, limit_tracks: int, seed: int = 1) -> list[Clip]:
    """Real negatives from the MTG-Jamendo sample (corpus_data/README.md) -
    Creative-Commons-sourced, genre-diverse, ~2100 tracks vs MUSDB18's 150.
    Broadens the narrow-negatives limitation flagged in this module's
    docstring rather than replacing MUSDB18 (independent real sources reduce
    the risk of the model keying on any one source's production style)."""
    files = sorted(glob.glob(os.path.join(root, "*.opus")))
    random.Random(seed).shuffle(files)
    clips = []
    for f in files[:limit_tracks]:
        dur = _probe_duration_sec(f)
        if dur is None:
            continue
        usable = max(0.0, dur - SEG_SEC - 10.0)
        if usable <= 0:
            continue
        step = usable / max(1, n_segments)
        for i in range(n_segments):
            clips.append(Clip(path=f, label=0, group="jamendo_" + os.path.basename(f),
                              offset=10.0 + i * step, source="jamendo"))
    return clips


def _probe_bitrate_k(path: str) -> int | None:
    """Nominal audio bitrate (kbps) of a file via ffprobe, used to match the
    MP3 round-trip on real negatives to the SONICS positives' encode path."""
    try:
        proc = subprocess.run(
            ["ffprobe", "-v", "quiet", "-select_streams", "a:0",
             "-show_entries", "stream=bit_rate", "-of", "csv=p=0", path],
            capture_output=True, timeout=15, text=True)
        return max(1, round(int(proc.stdout.strip()) / 1000))
    except Exception:
        return None


def build_corpus(root: str = _ROOT, n_fake: int = 300, n_real_seg: int = 4,
                 real_tracks: int = 100, val_frac: float = 0.2, seed: int = 0,
                 jamendo_root: str | None = _JAMENDO_ROOT, n_jamendo_tracks: int = 300,
                 codec_match: bool = False, codec_match_k: int = 0):
    """Return (train_clips, val_clips), grouped-split so no source track spans both.

    `jamendo_root=None` (or a missing directory) falls back to MUSDB18-only,
    matching this function's pre-2026-07-17 behavior.

    `codec_match=True` breaks the source/codec confound (audit AI #1) by
    re-encoding every *real* negative through the MP3 path the SONICS
    positives already took, so the codec can't be a shortcut. The target
    bitrate is `codec_match_k` (kbps) when given, else probed from the first
    SONICS positive, else 128. Off by default (the retrain is opt-in and the
    corpus data is git-ignored); the learned detector stays
    `intervention_validated:false` regardless."""
    fake = _fake_clips(root, n_fake, seed=seed)
    real = _musdb_clips(root, n_real_seg, real_tracks)
    if jamendo_root and os.path.isdir(jamendo_root):
        real += _jamendo_clips(jamendo_root, n_real_seg, n_jamendo_tracks, seed=seed + 1)
    if codec_match:
        probed = _probe_bitrate_k(fake[0].path) if fake else None
        k = codec_match_k or probed or 128
        for c in real:
            c.codec_match_mp3_k = int(k)
    clips = fake + real
    groups = sorted({c.group for c in clips})
    rng = random.Random(seed)
    rng.shuffle(groups)
    n_val = max(1, int(len(groups) * val_frac))
    val_groups = set(groups[:n_val])
    train = [c for c in clips if c.group not in val_groups]
    val = [c for c in clips if c.group in val_groups]
    return train, val
