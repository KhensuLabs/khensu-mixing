"""Does the CLAP AI probe detect generation, or recognise SONICS? (2026-11-18)

Every clip - AI and human alike - is cut to the same 10 s excerpt, made
mono at 16 kHz (FakeMusicCaps' own format) and round-tripped through the same
128 kb/s MP3 before it is embedded, so neither length, bandwidth nor codec
can separate the sources.

Sets:
  sonics          SONICS songs (Suno, Udio)               - AI, the shipped probe's training source
  fmc             FakeMusicCaps (MusicGen, MusicLDM,
                  AudioLDM2, Stable Audio Open, Mustango)  - AI, generators the probe never saw
  human_jamendo   MTG-Jamendo                              - human, the probe's training source
  human_archive   archive.org genre corpus (AI-tagged
                  tracks left out)                         - human, unseen
  human_lossless  the genre corpus's lossless subset       - human, unseen

    local-services/.venv/Scripts/python scripts/eval_ai_cross_dataset.py embed [--per-set N]
    local-services/.venv/Scripts/python scripts/eval_ai_cross_dataset.py eval

`eval` reports: the shipped probe on every set; then probes trained on one
AI dataset and tested on the other (with human sources the probe never saw);
then leave-one-generator-out across all seven generators. Results go to
bench-results/ai_cross_dataset.json.
"""
from __future__ import annotations

import argparse
import json
import random
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "local-services"))
sys.path.insert(0, str(ROOT / "scripts"))

from train_clap_probes import _ai_tagged, _embed, _load, _save

CACHE = ROOT / "build" / "clap_probes"
OUT = ROOT / "bench-results" / "ai_cross_dataset.json"
EXCERPT_S = 10.0
SONICS = ROOT / "Model Weights" / "fake_songs" / "fake_songs" / "fake_songs"
FMC = ROOT / "corpus_data" / "fakemusiccaps"
JAMENDO = ROOT / "corpus_data" / "mtg_jamendo_real" / "extracted"
ARCHIVE = ROOT / "corpus_data" / "genre_corpus_archive_org"
LOSSLESS = ROOT / "corpus_data" / "genre_corpus_lossless"


def _sonics_gen(p: Path) -> str:
    parts = p.stem.split("_")
    return parts[2] if len(parts) >= 3 else "unknown"


def _fmc_gen(p: Path) -> str:
    # FakeMusicCaps keeps each generator's clips in its own folder.
    rel = p.relative_to(FMC).parts
    return next((part for part in rel[:-1] if part.lower() not in ("fakemusiccaps",)), "unknown")


def items(per_set: int) -> list[tuple[str, str, int, str]]:
    """(path, set, label 1 = AI, generator/source)."""
    rng = random.Random(0)
    out: list[tuple[str, str, int, str]] = []

    def take(files, n):
        files = sorted(files)
        rng.shuffle(files)
        return files[:n]

    by_gen: dict[str, list[Path]] = {}
    for f in SONICS.glob("*.mp3"):
        by_gen.setdefault(_sonics_gen(f), []).append(f)
    for gen, files in sorted(by_gen.items()):
        out += [(str(p), "sonics", 1, gen) for p in take(files, per_set // max(1, len(by_gen)))]
    by_gen = {}
    for f in FMC.rglob("*.wav"):
        if not f.name.startswith("._"):  # macOS metadata in the archive, not audio
            by_gen.setdefault(_fmc_gen(f), []).append(f)
    for gen, files in sorted(by_gen.items()):
        out += [(str(p), "fmc", 1, gen) for p in take(files, per_set // max(1, len(by_gen)))]
    out += [(str(p), "human_jamendo", 0, "jamendo") for p in take(JAMENDO.glob("*.opus"), per_set)]
    arch = [p for g in ("*.mp3", "*.ogg") for p in ARCHIVE.rglob(g)]
    human_arch = [p for p in take(arch, per_set + 50) if not _ai_tagged(str(p))][:per_set]
    out += [(str(p), "human_archive", 0, "archive") for p in human_arch]
    loss = [p for g in ("*.flac", "*.wav") for p in LOSSLESS.rglob(g)]
    out += [(str(p), "human_lossless", 0, "lossless") for p in take(loss, per_set)]
    return out


def _excerpt(src: str, tmp: Path) -> str | None:
    """The middle EXCERPT_S of ``src``, mono at 16 kHz, through 128 kb/s MP3.

    FakeMusicCaps is 16 kHz mono: full-band human clips would be told apart
    by their bandwidth alone, so every set is brought down to the same."""
    try:
        dur = float(subprocess.run(
            ["ffprobe", "-v", "error", "-show_entries", "format=duration", "-of", "csv=p=0", src],
            capture_output=True, text=True, timeout=60, check=False).stdout.strip() or 0)
    except (OSError, ValueError, subprocess.TimeoutExpired):
        return None
    start = max(0.0, (dur - EXCERPT_S) / 2)
    out = tmp / "x.mp3"
    r = subprocess.run(["ffmpeg", "-y", "-v", "error", "-ss", f"{start:.2f}", "-t", f"{EXCERPT_S}", "-i", src,
                        "-ac", "1", "-ar", "16000", "-b:a", "128k", str(out)],
                       capture_output=True, timeout=120, check=False)
    return str(out) if r.returncode == 0 and out.exists() else None


def embed(per_set: int) -> None:
    path = CACHE / "x10_emb.npz"
    keys, embs = _load(path)
    done = set(keys)
    todo = [it for it in items(per_set) if it[0] not in done]
    print(f"{len(done)} embedded, {len(todo)} to go", flush=True)
    with tempfile.TemporaryDirectory(prefix="khensu_x10_") as td:
        for n, (p, _s, _l, _g) in enumerate(todo, 1):
            clip = _excerpt(p, Path(td))
            if clip is None:
                print(f"skip {Path(p).name}: excerpt failed", flush=True)
            e = _embed(clip) if clip else None
            if e is not None:
                keys.append(p)
                embs.append(e)
            if n % 100 == 0 or n == len(todo):
                _save(path, keys, embs)
                print(f"{n}/{len(todo)}", flush=True)


def _fit(X, y):
    from sklearn.linear_model import LogisticRegression
    from sklearn.preprocessing import StandardScaler

    sc = StandardScaler().fit(X)
    m = LogisticRegression(C=0.1, max_iter=4000).fit(sc.transform(X), y)
    return lambda Z: m.predict_proba(sc.transform(Z))[:, 1]


def evaluate(per_set: int) -> dict:
    from khensu_services.analysis.learned.clap_probes import (
        AI_FILE,
        ai_from_embedding,
        load_probe,
    )
    from sklearn.metrics import roc_auc_score

    meta = {p: (s, lab, g) for p, s, lab, g in items(per_set)}
    keys, embs = _load(CACHE / "x10_emb.npz")
    rows = [(k, e) for k, e in zip(keys, embs) if k in meta]
    X = np.stack([e for _, e in rows])
    sets = np.array([meta[k][0] for k, _ in rows])
    y = np.array([meta[k][1] for k, _ in rows])
    gens = np.array([meta[k][2] for k, _ in rows])
    report: dict = {"n": {s: int((sets == s).sum()) for s in sorted(set(sets))}}

    # 1. The shipped probe, unchanged.
    probe = load_probe(AI_FILE)
    shipped = np.array([ai_from_embedding(e, probe)["probability"] for e in X])
    human_unseen = np.isin(sets, ["human_archive", "human_lossless"])
    report["shipped"] = {
        "scored_ai": {s: round(float((shipped[sets == s] >= 0.5).mean()), 3) for s in sorted(set(sets))},
        "scored_ai_by_generator": {g: round(float((shipped[(gens == g) & (y == 1)] >= 0.5).mean()), 3)
                                   for g in sorted(set(gens[y == 1]))},
        "auc_vs_unseen_human": {s: round(float(roc_auc_score(
            np.r_[np.ones((sets == s).sum()), np.zeros(human_unseen.sum())],
            np.r_[shipped[sets == s], shipped[human_unseen]])), 3) for s in ("sonics", "fmc")},
    }

    # 2. Train on one AI dataset (+ Jamendo), test on the other (+ unseen humans).
    jam = sets == "human_jamendo"
    cross = {}
    for train_ai, test_ai in (("sonics", "fmc"), ("fmc", "sonics")):
        tr = (sets == train_ai) | jam
        te = (sets == test_ai) | human_unseen
        score = _fit(X[tr], y[tr])(X[te])
        cross[f"train_{train_ai}_test_{test_ai}"] = {
            "auc": round(float(roc_auc_score(y[te], score)), 3),
            "ai_scored_ai": round(float((score[y[te] == 1] >= 0.5).mean()), 3),
            "human_scored_ai": round(float((score[y[te] == 0] >= 0.5).mean()), 3),
        }
    report["cross_dataset"] = cross

    # 3. Everything but one generator (all humans split in half by source), test on it.
    rng = np.random.default_rng(0)
    human_idx = np.where(y == 0)[0]
    rng.shuffle(human_idx)
    h_train, h_test = human_idx[: len(human_idx) // 2], human_idx[len(human_idx) // 2:]
    logo = {}
    for g in sorted(set(gens[y == 1])):
        tr = np.r_[np.where((y == 1) & (gens != g))[0], h_train]
        te = np.r_[np.where((y == 1) & (gens == g))[0], h_test]
        score = _fit(X[tr], y[tr])(X[te])
        logo[g] = {"auc": round(float(roc_auc_score(y[te], score)), 3),
                   "ai_scored_ai": round(float((score[y[te] == 1] >= 0.5).mean()), 3),
                   "human_scored_ai": round(float((score[y[te] == 0] >= 0.5).mean()), 3)}
    report["leave_one_generator_out"] = logo
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(json.dumps(report, indent=1), encoding="utf-8")
    print(json.dumps(report, indent=1))
    return report


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("mode", choices=["embed", "eval"])
    ap.add_argument("--per-set", type=int, default=500)
    a = ap.parse_args()
    if a.mode == "embed":
        embed(a.per_set)
    evaluate(a.per_set)


if __name__ == "__main__":
    main()
