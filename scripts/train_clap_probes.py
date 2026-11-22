"""Train the CLAP mood and AI-generation probes (NEW-9, rest).

The genre probe (``train_genre_probe.py``) showed that standardised logistic
heads on the service's CLAP window embeddings (``learned.genre_probe.
window_embeddings``: three 10 s windows from the middle, mean-pooled) beat the
hand-built features. This does the same for the two other NEW-9 heads:

* **mood** - MTG-Jamendo's 13 mood/theme tags (``learned/mood_tagger.py``'s
  ``MOOD_TAGS``) on the same tracks and the SAME train / validation split as
  the feature-based tagger (sorted track ids, seed 0, 20 % held out), so its
  per-tag validation figures (``resources/models/mood_tagger.json``) are a
  like-for-like baseline. Per-tag thresholds are fitted on the training split
  (best F1), as the tagger's are.
* **ai** - AI-generated (SONICS: Suno and Udio songs) vs human-made
  (MTG-Jamendo). Both sides are cut to the same 40 s middle excerpt and
  round-tripped through the same MP3 encode (128 kb/s, 44.1 kHz) before they
  are embedded, so the probe cannot separate them by the final codec alone
  (audit AI #1). Scored by 5-fold cross-validation grouped by track, and by
  training on one generator and testing on the other (does it generalise to
  an unseen generator?). The real side is still one source (Jamendo, Opus
  before the round trip): a probe can still learn "Jamendo vs SONICS", which
  is why it ships as experimental evidence, never a verdict.

    local-services/.venv/Scripts/python scripts/train_clap_probes.py embed-mood
    local-services/.venv/Scripts/python scripts/train_clap_probes.py embed-ai
    local-services/.venv/Scripts/python scripts/train_clap_probes.py fit
    local-services/.venv/Scripts/python scripts/train_clap_probes.py unseen-human   # false alarms on an unseen human source

Embeddings are cached in build/clap_probes/ (resumable); ``fit`` writes
resources/models/clap_mood_probe.json and clap_ai_probe.json.
"""
from __future__ import annotations

import argparse
import datetime as _dt
import json
import random
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "local-services"))

JAMENDO = ROOT / "corpus_data" / "mtg_jamendo_real" / "extracted"
TAGS_TSV = ROOT / "corpus_data" / "mtg_jamendo_tags" / "autotagging_moodtheme.tsv"
SONICS = ROOT / "Model Weights" / "fake_songs" / "fake_songs" / "fake_songs"
# Human-made music from a source the AI probe never saw (unseen-human).
ARCHIVE = ROOT / "corpus_data" / "genre_corpus_archive_org"
UNSEEN_HUMAN_N = 300
# Provenance markers in a file's tags: such a track is AI-made, not human.
AI_MARKERS = (b"suno.com", b"udio.com", b"made with suno", b"suno ai", b"mubert.com", b"soundraw.io", b"aiva.ai")
CACHE = ROOT / "build" / "clap_probes"
MODELS = ROOT / "local-services" / "resources" / "models"
MOOD_OUT = MODELS / "clap_mood_probe.json"
AI_OUT = MODELS / "clap_ai_probe.json"
AI_PER_CLASS = 1600
# EXP-4: the shipped probe (SONICS vs Jamendo) called 53% of unseen human
# lossless tracks AI and 32% of the archive corpus (bench-results/
# ai_cross_dataset.json). The broad set adds every FakeMusicCaps generator on
# the AI side and archive.org music on the human side, and band-limited
# copies of human tracks (16 kHz mono, FakeMusicCaps' own format) so
# bandwidth cannot stand in for "AI".
FMC = ROOT / "corpus_data" / "fakemusiccaps"
LOSSLESS = ROOT / "corpus_data" / "genre_corpus_lossless"
ARCHIVE_TRAIN_N = 600
BANDLIMITED_HUMAN_N = 800
BANDLIMIT_SUFFIX = "#16k"
EXCERPT_S = 40.0
C_GRID = (0.01, 0.03, 0.1, 0.3, 1.0)


def _save(path: Path, keys: list[str], embs: list[np.ndarray], **extra) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(".tmp.npz")
    np.savez(tmp, keys=np.array(keys), embs=np.stack(embs) if embs else np.zeros((0, 512)), **extra)
    tmp.replace(path)


def _load(path: Path) -> tuple[list[str], list[np.ndarray]]:
    if not path.exists():
        return [], []
    d = np.load(path, allow_pickle=False)
    return [str(k) for k in d["keys"]], [np.asarray(e) for e in d["embs"]]


def _embed(path: str) -> np.ndarray | None:
    from khensu_services.analysis.learned.genre_probe import pool, window_embeddings

    try:
        w = window_embeddings(path)
    except Exception as exc:  # noqa: BLE001 - a broken file is skipped, not fatal
        print(f"skip {Path(path).name}: {exc}", flush=True)
        return None
    if w is None or len(w) == 0:
        return None
    return pool(np.asarray(w), "mean")


# ---- mood -------------------------------------------------------------------

def mood_items() -> tuple[list[str], dict[str, list[str]], set[str]]:
    """(track ids, {id: tags}, validation ids) exactly as mood_tagger.train splits them."""
    from khensu_services.analysis.learned.mood_tagger import _load_tag_overlap

    tagged = _load_tag_overlap(str(JAMENDO), str(TAGS_TSV))
    ids = sorted(tagged)
    rng = np.random.default_rng(0)
    shuffled = list(ids)
    rng.shuffle(shuffled)
    val = set(shuffled[: max(1, int(len(shuffled) * 0.2))])
    return ids, tagged, val


def embed_mood() -> None:
    ids, _tagged, _val = mood_items()
    out = CACHE / "mood_emb.npz"
    keys, embs = _load(out)
    done = set(keys)
    todo = [i for i in ids if i not in done]
    print(f"mood: {len(done)} embedded, {len(todo)} to go", flush=True)
    t0 = time.time()
    for n, tid in enumerate(todo, 1):
        e = _embed(str(JAMENDO / f"{tid}.opus"))
        if e is not None:
            keys.append(tid)
            embs.append(e)
        if n % 200 == 0 or n == len(todo):
            _save(out, keys, embs)
            print(f"mood {n}/{len(todo)} ({(time.time() - t0) / n:.2f} s/track)", flush=True)


# ---- ai ---------------------------------------------------------------------

def _generator(name: str) -> str:
    parts = Path(name).stem.split("_")
    return parts[2] if len(parts) >= 3 else "unknown"


def ai_items(seed: int = 0) -> list[tuple[str, int, str]]:
    """(path, label 1 = AI, group) - balanced across the SONICS generators."""
    rng = random.Random(seed)
    fakes: dict[str, list[str]] = {}
    for f in sorted(SONICS.glob("*.mp3")):
        fakes.setdefault(_generator(f.name), []).append(str(f))
    per_gen = AI_PER_CLASS // max(1, len(fakes))
    items = []
    for gen, files in sorted(fakes.items()):
        rng.shuffle(files)
        items += [(p, 1, gen) for p in files[:per_gen]]
    reals = sorted(str(p) for p in JAMENDO.glob("*.opus"))
    rng.shuffle(reals)
    jam = reals[:AI_PER_CLASS]
    items += [(p, 0, "jamendo") for p in jam]
    # EXP-4 broad set.
    for gen_dir in sorted(d for d in FMC.iterdir() if d.is_dir()) if FMC.is_dir() else []:
        items += [(str(p), 1, gen_dir.name) for p in sorted(gen_dir.glob("*.wav"))]
    # Nothing from an album either held-out human set holds (unseen-human,
    # and the lossless originals of the same corpus): a song's lossy copy in
    # training would leak its lossless original's score.
    def _album(path: str) -> tuple[str, str]:
        q = Path(path)
        return q.parent.name, q.name.split("__", 1)[0]
    held_out = {_album(p) for p in unseen_human_items()} | {_album(p) for p in lossless_items()}
    archive = [p for p in sorted({str(p) for g in ("*.mp3", "*.ogg") for p in ARCHIVE.rglob(g)})
               if _album(p) not in held_out]
    rng.shuffle(archive)
    archive = [p for p in archive if not _ai_tagged(p)][:ARCHIVE_TRAIN_N]
    items += [(p, 0, "archive") for p in archive]
    humans = jam + archive
    rng.shuffle(humans)
    items += [(p + BANDLIMIT_SUFFIX, 0, "bandlimited") for p in humans[:BANDLIMITED_HUMAN_N]]
    return items


def _codec_matched(src: str, tmp: Path) -> str | None:
    """The middle EXCERPT_S of `src`, round-tripped through 128 kb/s MP3. A
    key ending in BANDLIMIT_SUFFIX is first cut to 16 kHz mono (FakeMusicCaps'
    format), then encoded the same way."""
    band_limit = src.endswith(BANDLIMIT_SUFFIX)
    if band_limit:
        src = src[: -len(BANDLIMIT_SUFFIX)]
    try:
        dur = float(subprocess.run(
            ["ffprobe", "-v", "error", "-show_entries", "format=duration", "-of", "csv=p=0", src],
            capture_output=True, text=True, timeout=60, check=False).stdout.strip() or 0)
    except (OSError, ValueError, subprocess.TimeoutExpired):
        return None
    start = max(0.0, (dur - EXCERPT_S) / 2)
    out = tmp / "x.mp3"
    limit = ["-af", "aresample=16000,pan=mono|c0=0.5*c0+0.5*c1,aresample=44100"] if band_limit else []
    r = subprocess.run(["ffmpeg", "-y", "-v", "error", "-ss", f"{start:.2f}", "-t", f"{EXCERPT_S}", "-i", src,
                        *limit, "-ac", "2", "-ar", "44100", "-b:a", "128k", str(out)], capture_output=True,
                       timeout=120, check=False)
    return str(out) if r.returncode == 0 and out.exists() else None


def embed_ai() -> None:
    items = ai_items()
    out = CACHE / "ai_emb.npz"
    keys, embs = _load(out)
    done = set(keys)
    todo = [it for it in items if it[0] not in done]
    print(f"ai: {len(done)} embedded, {len(todo)} to go", flush=True)
    t0 = time.time()
    with tempfile.TemporaryDirectory(prefix="khensu_clap_ai_") as td:
        for n, (path, _label, _group) in enumerate(todo, 1):
            matched = _codec_matched(path, Path(td))
            e = _embed(matched) if matched else None
            if e is not None:
                keys.append(path)
                embs.append(e)
            if n % 200 == 0 or n == len(todo):
                _save(out, keys, embs)
                print(f"ai {n}/{len(todo)} ({(time.time() - t0) / n:.2f} s/track)", flush=True)


# ---- fit --------------------------------------------------------------------

def _logreg(c: float):
    from sklearn.linear_model import LogisticRegression

    return LogisticRegression(C=c, max_iter=4000, class_weight="balanced")


def _head(model, scaler) -> dict:
    return {"coef": model.coef_[0].astype(float).round(6).tolist(), "intercept": float(model.intercept_[0])}


def _best_f1_threshold(p: np.ndarray, y: np.ndarray) -> float:
    from sklearn.metrics import f1_score

    grid = np.linspace(0.05, 0.95, 91)
    return float(max(grid, key=lambda t: f1_score(y, p >= t, zero_division=0)))


def fit_mood() -> dict:
    from khensu_services.analysis.learned.mood_tagger import MOOD_TAGS
    from sklearn.metrics import balanced_accuracy_score, f1_score, roc_auc_score
    from sklearn.model_selection import cross_val_score
    from sklearn.preprocessing import StandardScaler

    ids, tagged, val = mood_items()
    keys, embs = _load(CACHE / "mood_emb.npz")
    index = {k: i for i, k in enumerate(keys)}
    use = [i for i in ids if i in index]
    X = np.stack([embs[index[i]] for i in use])
    Y = np.array([[1.0 if t in tagged[i] else 0.0 for t in MOOD_TAGS] for i in use])
    is_val = np.array([i in val for i in use])
    scaler = StandardScaler().fit(X[~is_val])
    Xtr, Xva = scaler.transform(X[~is_val]), scaler.transform(X[is_val])

    baseline = json.loads((MODELS / "mood_tagger.json").read_text(encoding="utf-8")).get("val_per_tag", {})
    heads, thresholds, per_tag = {}, {}, {}
    for j, tag in enumerate(MOOD_TAGS):
        ytr, yva = Y[~is_val, j], Y[is_val, j]
        c = max(C_GRID, key=lambda cc: cross_val_score(_logreg(cc), Xtr, ytr, cv=3,
                                                     scoring="balanced_accuracy").mean())
        m = _logreg(c).fit(Xtr, ytr)
        thr = _best_f1_threshold(m.predict_proba(Xtr)[:, 1], ytr)
        pva = m.predict_proba(Xva)[:, 1]
        heads[tag] = _head(m, scaler)
        thresholds[tag] = round(thr, 3)
        b = baseline.get(tag, {})
        per_tag[tag] = {
            "val_positives": int(yva.sum()),
            "balanced_accuracy": round(float(balanced_accuracy_score(yva, pva >= thr)), 3),
            "f1": round(float(f1_score(yva, pva >= thr, zero_division=0)), 3),
            "roc_auc": round(float(roc_auc_score(yva, pva)), 3),
            "baseline_balanced_accuracy": b.get("balanced_accuracy"),
            "baseline_f1": b.get("f1"),
            "C": c,
        }
        print(f"{tag:11s} bal_acc {per_tag[tag]['balanced_accuracy']:.3f} (tagger {b.get('balanced_accuracy')}) "
              f"f1 {per_tag[tag]['f1']:.3f} (tagger {b.get('f1')}) auc {per_tag[tag]['roc_auc']:.3f}", flush=True)
    probe = {
        "format": 1, "kind": "mood", "embedding": "clap_window_mean", "tags": MOOD_TAGS,
        "scaler_mean": scaler.mean_.round(6).tolist(), "scaler_scale": scaler.scale_.round(6).tolist(),
        "heads": heads, "thresholds": thresholds,
        "n_train": int((~is_val).sum()), "n_val": int(is_val.sum()), "val_per_tag": per_tag,
        "provenance": "MTG-Jamendo mood/theme tags, the feature tagger's own train / validation split "
                      "(scripts/train_clap_probes.py)",
        "trained": _dt.datetime.now(_dt.timezone.utc).date().isoformat(), "intervention_validated": False,
    }
    MOOD_OUT.write_text(json.dumps(probe), encoding="utf-8")
    _pin(MOOD_OUT)
    return per_tag


def fit_ai() -> dict:
    from sklearn.metrics import roc_auc_score
    from sklearn.model_selection import StratifiedKFold
    from sklearn.preprocessing import StandardScaler

    items = {p: (lab, grp) for p, lab, grp in ai_items()}
    keys, embs = _load(CACHE / "ai_emb.npz")
    use = [(k, e) for k, e in zip(keys, embs) if k in items]
    X = np.stack([e for _, e in use])
    y = np.array([items[k][0] for k, _ in use])
    groups = np.array([items[k][1] for k, _ in use])

    def auc_of(tr, te, c):
        sc = StandardScaler().fit(X[tr])
        m = _logreg(c).fit(sc.transform(X[tr]), y[tr])
        return float(roc_auc_score(y[te], m.predict_proba(sc.transform(X[te]))[:, 1]))

    folds = list(StratifiedKFold(5, shuffle=True, random_state=0).split(X, y))
    c_best = max(C_GRID, key=lambda c: np.mean([auc_of(tr, te, c) for tr, te in folds]))
    cv_auc = float(np.mean([auc_of(tr, te, c_best) for tr, te in folds]))
    print(f"ai: 5-fold AUC {cv_auc:.3f} (C={c_best}, n={len(y)})", flush=True)

    # Unseen generator: train on the real side + one generator, test on the other.
    unseen = {}
    humans = {"jamendo", "archive", "bandlimited"}
    gens = sorted(set(groups) - humans)
    real_idx = np.where(np.isin(groups, list(humans)))[0]
    rng = np.random.default_rng(0)
    rng.shuffle(real_idx)
    half = len(real_idx) // 2
    for g in gens:
        others = [o for o in gens if o != g]
        tr = np.concatenate([real_idx[:half], np.where(np.isin(groups, others))[0]])
        te = np.concatenate([real_idx[half:], np.where(groups == g)[0]])
        unseen[g] = round(auc_of(tr, te, c_best), 3)
        print(f"ai: trained without {g}, AUC on {g} vs held-out real {unseen[g]:.3f}", flush=True)

    sc = StandardScaler().fit(X)
    m = _logreg(c_best).fit(sc.transform(X), y)
    probe = {
        "format": 1, "kind": "ai", "embedding": "clap_window_mean",
        "scaler_mean": sc.mean_.round(6).tolist(), "scaler_scale": sc.scale_.round(6).tolist(),
        "head": _head(m, sc), "C": c_best,
        "cv_auc": round(cv_auc, 3), "unseen_generator_auc": unseen,
        "n": len(y), "n_ai": int(y.sum()), "generators": gens,
        "codec_matched": "both sides: 40 s middle excerpt, 128 kb/s MP3 round trip",
        "limitation": "Trained on seven generators (SONICS: Suno, Udio; FakeMusicCaps: MusicGen, MusicLDM, "
                      "AudioLDM2, Stable Audio Open, Mustango) against MTG-Jamendo and archive.org music, with "
                      "band-limited human copies so bandwidth is not a cue (EXP-4). Held out: each generator "
                      "in turn (unseen_generator_auc) and two human sources (unseen_source_false_positive_rate, "
                      "lossless_false_positive_rate). A generator unlike all seven can still pass as human. "
                      "Experimental evidence, never a verdict.",
        "trained": _dt.datetime.now(_dt.timezone.utc).date().isoformat(), "intervention_validated": False,
    }
    AI_OUT.write_text(json.dumps(probe), encoding="utf-8")
    _pin(AI_OUT)
    if (CACHE / "unseen_human_emb.npz").exists():
        score_unseen_human()
    if (CACHE / "lossless_human_emb.npz").exists():
        score_lossless()
    return {"cv_auc": cv_auc, "unseen": unseen}


def _ai_tagged(path: str) -> bool:
    """True when the file's tags name an AI generation service (a Suno song
    uploaded to archive.org is not human-made music)."""
    try:
        with open(path, "rb") as f:
            head = f.read(512 * 1024)
            f.seek(0, 2)
            f.seek(max(0, f.tell() - 128 * 1024))
            tail = f.read()
    except OSError:
        return False
    blob = (head + tail).lower()
    return any(m in blob for m in AI_MARKERS)


def unseen_human_items() -> list[str]:
    files = sorted({str(p) for g in ("*.mp3", "*.ogg") for p in ARCHIVE.rglob(g)})
    random.Random(0).shuffle(files)
    return files[:UNSEEN_HUMAN_N]


def embed_unseen_human() -> None:
    out = CACHE / "unseen_human_emb.npz"
    keys, embs = _load(out)
    todo = [p for p in unseen_human_items() if p not in set(keys)]
    print(f"unseen-human: {len(keys)} embedded, {len(todo)} to go", flush=True)
    with tempfile.TemporaryDirectory(prefix="khensu_clap_unseen_") as td:
        for n, path in enumerate(todo, 1):
            matched = _codec_matched(path, Path(td))
            e = _embed(matched) if matched else None
            if e is not None:
                keys.append(path)
                embs.append(e)
            if n % 50 == 0 or n == len(todo):
                _save(out, keys, embs)
    score_unseen_human()


def score_unseen_human() -> dict:
    """The shipped probe's false-alarm rate on human music from a source it
    never saw, through the same excerpt and MP3 round trip. Tracks whose tags
    name an AI service are reported apart, not counted as human."""
    from khensu_services.analysis.learned.clap_probes import ai_from_embedding

    probe = json.loads(AI_OUT.read_text(encoding="utf-8"))
    keys, embs = _load(CACHE / "unseen_human_emb.npz")
    human, tagged = [], []
    for k, e in zip(keys, embs):
        (tagged if _ai_tagged(k) else human).append(ai_from_embedding(e, probe)["probability"])
    human = np.array(human)
    rate = float((human >= 0.5).mean()) if len(human) else None
    probe["unseen_source_false_positive_rate"] = round(rate, 3) if rate is not None else None
    probe["unseen_source_note"] = (
        f"{len(human)} human-made archive.org genre-corpus tracks from albums the probe never saw, same excerpt "
        f"and MP3 round trip: {100 * rate:.1f}% scored at or over 0.5, median {float(np.median(human)):.3f}. "
        f"{len(tagged)} more tracks there carry AI-service tags (suno.com etc.) and are not counted as human; "
        f"{sum(p >= 0.5 for p in tagged)} of them scored at or over 0.5.")
    AI_OUT.write_text(json.dumps(probe), encoding="utf-8")
    _pin(AI_OUT)
    print("unseen-human:", probe["unseen_source_note"], flush=True)
    return {"rate": rate, "n_human": len(human), "n_tagged": len(tagged)}


def lossless_items() -> list[str]:
    manifest = json.loads((LOSSLESS / "manifest.json").read_text(encoding="utf-8"))
    return [str(LOSSLESS / g / t["file"]) for g, ts in manifest.items() for t in ts
            if t.get("file") and (LOSSLESS / g / t["file"]).is_file()]


def embed_lossless() -> None:
    """EXP-4: the lossless originals - human music no probe trains on - through
    the same excerpt and round trip, for the held-out false-alarm rate."""
    out = CACHE / "lossless_human_emb.npz"
    keys, embs = _load(out)
    todo = [p for p in lossless_items() if p not in set(keys)]
    print(f"lossless: {len(keys)} embedded, {len(todo)} to go", flush=True)
    with tempfile.TemporaryDirectory(prefix="khensu_clap_lossless_") as td:
        for n, path in enumerate(todo, 1):
            matched = _codec_matched(path, Path(td))
            e = _embed(matched) if matched else None
            if e is not None:
                keys.append(path)
                embs.append(e)
            if n % 50 == 0 or n == len(todo):
                _save(out, keys, embs)


def score_lossless() -> dict:
    from khensu_services.analysis.learned.clap_probes import ai_from_embedding

    probe = json.loads(AI_OUT.read_text(encoding="utf-8"))
    _keys, embs = _load(CACHE / "lossless_human_emb.npz")
    p = np.array([ai_from_embedding(e, probe)["probability"] for e in embs])
    rate = float((p >= 0.5).mean()) if len(p) else None
    probe["lossless_false_positive_rate"] = round(rate, 3) if rate is not None else None
    probe["lossless_note"] = (f"{len(p)} lossless human originals (genre corpus, never trained on): "
                              f"{100 * rate:.1f}% scored at or over 0.5, median {float(np.median(p)):.3f}.")
    AI_OUT.write_text(json.dumps(probe), encoding="utf-8")
    _pin(AI_OUT)
    print("lossless:", probe["lossless_note"], flush=True)
    return {"rate": rate, "n": len(p)}


def _pin(path: Path) -> None:
    """The new pin goes into model_pins.json as a reviewed repo diff: a bundled
    model without a matching pin does not load."""
    from khensu_services.analysis.model_manifest import pin_model

    print(f"pinned {path.name}: {pin_model(path.name.split('.', 1)[0], path)}", flush=True)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("mode", choices=["embed-mood", "embed-ai", "fit", "fit-mood", "fit-ai", "unseen-human",
                                     "lossless"])
    a = ap.parse_args()
    if a.mode == "embed-mood":
        embed_mood()
    elif a.mode == "embed-ai":
        embed_ai()
    elif a.mode == "unseen-human":
        embed_unseen_human()
    elif a.mode == "lossless":
        embed_lossless()
        score_lossless()
    if a.mode in ("fit", "fit-mood"):
        fit_mood()
    if a.mode in ("fit", "fit-ai"):
        fit_ai()


if __name__ == "__main__":
    main()
