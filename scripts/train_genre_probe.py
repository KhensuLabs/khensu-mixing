"""Train the CLAP genre probe (NEW-9, GEN-1).

Embeds every track of the on-disk genre corpus, plus any extra corpora
(``--extra`` lists of ``genre<TAB>path``, e.g. FMA from
``scripts/genre_corpus_fma.py``), with the service's own
``learned.genre_probe.window_embeddings`` (cached per corpus and window
setting, so a rerun only embeds new tracks). Fits two standardised
logistic-regression heads - genre family and catalog genre - and writes
``resources/genre_norms/genre_probe_clap.json``.

Evaluation is stratified 4-fold cross-validation over the archive.org
corpus (whole songs, like a user's track); extra corpora are always on the
training side, never scored. By default they train only the family head, at
sample weight 0.1: FMA at full weight, or in the genre head, lowered the
archive scores (its tags disagree with the archive's at subgenre level,
docs/2026-11-15-genre-accuracy.md). The regularisation is chosen on the same folds,
so the figures are slightly optimistic. Subgenres the probe keeps confusing
with each other (cross-validated confusion rate >= ``--group-rate``, same
family) are grouped; a group is scored as one candidate (GEN-1a).
``--clap music`` embeds with LAION's music-trained checkpoint instead, into
separate caches, for comparison only: the service loads the general one
(GEN-2, docs/2026-11-15-genre-accuracy.md section 6).

Run from the repo root with the service venv:

    local-services/.venv/Scripts/python scripts/train_genre_probe.py \
        --extra build/genre_probe/fma_items.tsv
"""
from __future__ import annotations

import argparse
import datetime as _dt
import json
import sys
import time
from collections import Counter
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "local-services"))

DEFAULT_CORPUS = ROOT / "corpus_data" / "genre_corpus_archive_org"
DEFAULT_CACHE_DIR = ROOT / "build" / "genre_probe"
CATALOG = ROOT / "resources" / "genre_norms" / "genre_targets.json"
MAX_WINDOWS = 12
C_GRID = (0.003, 0.01, 0.03)
MAX_GROUP = 4


def _families() -> tuple[dict[str, str], dict[str, str]]:
    from khensu_services.analysis.learned.genre_probe import FAMILY_FOLD

    cat = json.loads(CATALOG.read_text(encoding="utf-8"))["genres"]
    parent = {g["id"]: str(g.get("parent", g["id"])) for g in cat}
    family = {gid: FAMILY_FOLD.get(p, p) for gid, p in parent.items()}
    return parent, family


def archive_items(corpus: Path) -> list[tuple[str, str]]:
    manifest = json.loads((corpus / "manifest.json").read_text(encoding="utf-8"))
    items = [(gid, corpus / gid / t["file"]) for gid, tracks in sorted(manifest.items()) for t in tracks]
    return [(g, str(p)) for g, p in items if p.exists()]


def tsv_items(path: Path) -> list[tuple[str, str]]:
    rows = [ln.split("\t") for ln in path.read_text(encoding="utf-8").splitlines() if ln.strip()]
    return [(g, p) for g, p in rows if Path(p).exists()]


def embed(items: list[tuple[str, str]], cache: Path, spec: dict) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """(labels, windows (n, MAX_WINDOWS, d), window counts) for ``items``."""
    from khensu_services.analysis.learned.genre_probe import window_embeddings

    done: dict[str, tuple[str, np.ndarray, int]] = {}
    if cache.exists():
        d = np.load(cache)
        for lab, p, e, n in zip(d["labels"], d["paths"], d["embs"], d["nwin"]):
            done[str(p)] = (str(lab), np.asarray(e), int(n))
    todo = [(g, p) for g, p in items if p not in done]
    print(f"{cache.name}: {len(items)} tracks, {len(todo)} to embed", flush=True)

    def save() -> None:
        cache.parent.mkdir(parents=True, exist_ok=True)
        np.savez(cache, labels=np.array([v[0] for v in done.values()]), paths=np.array(list(done)),
                 embs=np.stack([v[1] for v in done.values()]).astype(np.float32),
                 nwin=np.array([v[2] for v in done.values()]))

    t0 = time.time()
    for i, (gid, path) in enumerate(todo):
        try:
            e = window_embeddings(path, spec)[:MAX_WINDOWS]
            buf = np.zeros((MAX_WINDOWS, e.shape[1]), np.float32)
            buf[:len(e)] = e
            done[path] = (gid, buf, len(e))
        except Exception as exc:  # noqa: BLE001 - an undecodable file is skipped
            print(f"skip {Path(path).name}: {type(exc).__name__}", flush=True)
        if i % 100 == 0:
            print(f"  {i}/{len(todo)} {time.time() - t0:.0f}s", flush=True)
            save()
    if todo:
        save()
    rows = [(done[p][0], done[p][1], done[p][2]) for _, p in items if p in done]
    return (np.array([r[0] for r in rows]), np.stack([r[1] for r in rows]), np.array([r[2] for r in rows]))


def features(windows: np.ndarray, nwin: np.ndarray, pooling: str) -> np.ndarray:
    from khensu_services.analysis.learned.genre_probe import pool

    return np.stack([pool(w[:max(1, n)], pooling) for w, n in zip(windows, nwin)])


def _fit_head(X: np.ndarray, y: np.ndarray, C: float, class_weight, weight: np.ndarray | None = None):
    """Standardise on the full-weight rows (the archive songs; extra clips
    are a different recording population) and fit a weighted regression."""
    from sklearn.linear_model import LogisticRegression
    from sklearn.preprocessing import StandardScaler

    w = np.ones(len(y)) if weight is None else weight
    sc = StandardScaler().fit(X[w >= 1.0])
    clf = LogisticRegression(max_iter=2000, C=C, class_weight=class_weight).fit(sc.transform(X), y, sample_weight=w)
    return sc, clf


def _proba(model, X: np.ndarray, classes: np.ndarray) -> np.ndarray:
    sc, clf = model
    P = clf.predict_proba(sc.transform(X))
    out = np.zeros((len(X), len(classes)))
    idx = {c: i for i, c in enumerate(classes)}
    for j, c in enumerate(clf.classes_):
        out[:, idx[c]] = P[:, j]
    return out


def cv_proba(X, y, train_only, folds_of, C, class_weight, classes, weight=None):
    """Out-of-fold probabilities for the eval rows (folds_of >= 0); rows
    with folds_of == -1 (extra corpora) always train."""
    from joblib import Parallel, delayed

    P = np.zeros((len(y), len(classes)))
    nfold = int(folds_of.max()) + 1

    def one(k):
        tr = (folds_of != k) | train_only
        te = folds_of == k
        w = None if weight is None else weight[tr]
        return k, _proba(_fit_head(X[tr], y[tr], C, class_weight, w), X[te], classes)

    for k, p in Parallel(n_jobs=nfold)(delayed(one)(k) for k in range(nfold)):
        P[folds_of == k] = p
    return P


def topk(P: np.ndarray, y: np.ndarray, classes: np.ndarray, k: int) -> float:
    order = np.argsort(-P, axis=1)[:, :k]
    return float(np.mean([y[i] in classes[order[i]] for i in range(len(y))]))


def derive_groups(P, y, classes, family, rate: float) -> list[list[str]]:
    """Union same-family genre pairs confused at >= rate (count / smaller
    class size), most confused first, keeping groups at most MAX_GROUP."""
    pred = classes[np.argmax(P, axis=1)]
    n = Counter(y)
    conf = Counter(tuple(sorted((t, p))) for t, p in zip(y, pred) if t != p)
    pairs = sorted(((c / min(n[a], n[b]), a, b) for (a, b), c in conf.items()
                    if n[a] and n[b] and family[a] == family[b]), reverse=True)
    group = {c: {c} for c in classes}
    for r, a, b in pairs:
        if r < rate or group[a] is group[b] or len(group[a] | group[b]) > MAX_GROUP:
            continue
        merged = group[a] | group[b]
        for g in merged:
            group[g] = merged
    uniq = {id(s): s for s in group.values() if len(s) > 1}
    return sorted(sorted(str(g) for g in s) for s in uniq.values())


def group_topk(P, y, classes, groups, k: int) -> float:
    gid = {c: i for i, grp in enumerate(groups) for c in grp}
    keys = [gid.get(c, c) for c in classes]
    hits = 0
    for i in range(len(y)):
        scores: dict = {}
        for j, key in enumerate(keys):
            scores[key] = scores.get(key, 0.0) + P[i, j]
        top = sorted(scores, key=lambda s: -scores[s])[:k]
        hits += gid.get(y[i], y[i]) in top
    return hits / len(y)


def _head_json(model) -> dict:
    sc, clf = model
    return {"classes": [str(c) for c in clf.classes_],
            "weights": np.round(clf.coef_, 5).tolist(),
            "bias": np.round(clf.intercept_, 5).tolist(),
            "mean": np.round(sc.mean_, 6).tolist(),
            "scale": np.round(sc.scale_, 6).tolist(),
            "C": float(clf.C)}


def main() -> int:
    ap = argparse.ArgumentParser(description="Train the CLAP genre probe.")
    ap.add_argument("--corpus", type=Path, default=DEFAULT_CORPUS)
    ap.add_argument("--extra", action="append", default=[],
                    help="genre<TAB>path list always used for training (repeatable), as PATH or "
                         "PATH:WEIGHT:HEADS (HEADS family|both); defaults --extra-weight/--extra-heads")
    ap.add_argument("--cache-dir", type=Path, default=DEFAULT_CACHE_DIR)
    ap.add_argument("--windows", choices=("middle", "spread"), default="spread")
    ap.add_argument("--max-windows", type=int, default=MAX_WINDOWS)
    ap.add_argument("--pooling", choices=("mean", "mean_std"), default="mean")
    ap.add_argument("--class-weight", choices=("none", "balanced"), default="none")
    ap.add_argument("--extra-weight", type=float, default=0.1,
                    help="sample weight of extra-corpus tracks in the family head")
    ap.add_argument("--extra-heads", choices=("family", "both"), default="family",
                    help="which heads the extra corpora train (their genre tags disagree with the archive's)")
    ap.add_argument("--group-rate", type=float, default=0.35)
    ap.add_argument("--c", type=float, nargs="*", default=list(C_GRID))
    ap.add_argument("--clap", choices=("general", "music"), default="general",
                    help="CLAP checkpoint: the service's general one, or LAION's music-trained one (comparison only)")
    ap.add_argument("--out", type=Path, default=ROOT / "resources" / "genre_norms" / "genre_probe_clap.json")
    args = ap.parse_args()

    from khensu_services.analysis import embeddings as emb
    from khensu_services.analysis.learned import genre_probe

    spec = {"windows": args.windows, "max_windows": min(args.max_windows, MAX_WINDOWS), "pooling": args.pooling}
    tag = f"{args.windows}{spec['max_windows']}"
    clap_pin = emb._clap_pin()
    if args.clap == "music":
        # The service always loads the general checkpoint; set the module's model
        # before anything embeds, and keep the music embeddings in their own caches.
        from khensu_services.analysis.artifacts import safe_weights
        from khensu_services.common.online import hf_downloads_allowed

        clap_pin = safe_weights.CLAP_MUSIC_WEIGHTS
        emb._clap_model = safe_weights.build_clap_model(hf_downloads_allowed(), device=emb._torch_device(),
                                                        pin=clap_pin)
        tag += "_music"
    parent, family = _families()
    labels_a, W_a, n_a = embed(archive_items(args.corpus), args.cache_dir / f"archive_{tag}.npz", spec)
    parts = [(labels_a, W_a, n_a, np.zeros(len(labels_a), bool), 1.0, True)]
    extra_counts = {}
    extra_setup = {}
    for spec_arg in args.extra:
        bits = spec_arg.rsplit(":", 2)
        if len(bits) != 3 or bits[2] not in ("family", "both"):
            bits = [spec_arg]
        path = Path(bits[0])
        w_extra = float(bits[1]) if len(bits) == 3 else args.extra_weight
        both = (bits[2] if len(bits) == 3 else args.extra_heads) == "both"
        lab, W, n = embed(tsv_items(path), args.cache_dir / f"{path.stem}_{tag}.npz", spec)
        parts.append((lab, W, n, np.ones(len(lab), bool), w_extra, both))
        extra_counts[path.stem] = len(lab)
        extra_setup[path.stem] = {"weight": w_extra, "heads": "both" if both else "family"}
    labels = np.concatenate([p[0] for p in parts])
    unknown = sorted(set(labels) - set(parent))
    if unknown:
        print(f"labels missing from the catalog: {unknown}", file=sys.stderr)
        return 1
    X = np.concatenate([features(p[1], p[2], args.pooling) for p in parts])
    is_extra = np.concatenate([p[3] for p in parts])
    weight = np.concatenate([np.full(len(p[0]), p[4]) for p in parts])
    in_genre = np.concatenate([np.full(len(p[0]), p[5]) for p in parts])

    # A genre needs 4 tracks overall; scored folds need 4 archive tracks.
    total = Counter(labels)
    keep = np.array([total[g] >= 4 for g in labels])
    labels, X, is_extra, weight, in_genre = labels[keep], X[keep], is_extra[keep], weight[keep], in_genre[keep]
    arch = Counter(labels[~is_extra])
    scored = ~is_extra & np.array([arch[g] >= 4 for g in labels])
    train_only = ~scored
    from sklearn.model_selection import StratifiedKFold

    folds_of = np.full(len(labels), -1)
    idx = np.where(scored)[0]
    for k, (_, te) in enumerate(StratifiedKFold(4, shuffle=True, random_state=0).split(idx, labels[idx])):
        folds_of[idx[te]] = k
    fam = np.array([family[g] for g in labels])
    eval_fam = Counter(fam[scored])
    majority = eval_fam.most_common(1)[0][1] / int(scored.sum())
    cw = None if args.class_weight == "none" else "balanced"
    print(f"training on {len(labels)} tracks ({int(train_only.sum())} train-only), {len(set(labels))} genres, "
          f"{len(set(fam))} families; scored {int(scored.sum())} archive tracks "
          f"(majority-family baseline {majority:.1%})", flush=True)

    # The genre head trains on the archive and the extras marked "both".
    gen_rows = in_genre
    gen_classes = np.array(sorted(set(labels[gen_rows])))
    fam_classes = np.array(sorted(set(fam)))
    best = {}
    heads = (("family", np.ones(len(labels), bool), fam, fam_classes), ("genre", gen_rows, labels, gen_classes))
    for head, rows, y_all, classes in heads:
        scores = {}
        for C in args.c:
            P = np.zeros((len(labels), len(classes)))
            P[rows] = cv_proba(X[rows], y_all[rows], train_only[rows], folds_of[rows], C, cw, classes,
                               weight[rows])
            P = P[scored]
            y = y_all
            scores[C] = (topk(P, y[scored], classes, 1), topk(P, y[scored], classes, 5), P)
            print(f"  {head} C={C:<5} top1 {scores[C][0]:.1%} top5 {scores[C][1]:.1%}", flush=True)
        C = max(scores, key=lambda c: scores[c][0])
        best[head] = (C, *scores[C])
    P_gen = best["genre"][3]
    groups = derive_groups(P_gen, labels[scored], gen_classes, family, args.group_rate)
    g1 = group_topk(P_gen, labels[scored], gen_classes, groups, 1)
    g5 = group_topk(P_gen, labels[scored], gen_classes, groups, 5)
    print(f"groups ({len(groups)}): {groups}")
    print(f"group top1 {g1:.1%} top5 {g5:.1%}")

    fam_model = _fit_head(X, fam, best["family"][0], cw, weight)
    gen_model = _fit_head(X[gen_rows], labels[gen_rows], best["genre"][0], cw, weight[gen_rows])
    pin = clap_pin
    probe = {
        "format": genre_probe.PROBE_FORMAT,
        "created": _dt.datetime.now(_dt.timezone.utc).date().isoformat(),
        "embedding": {"model": emb.CLAP_REPO, "checkpoint": clap_pin.filename,
                      "pin": str(pin) if pin is not None else None,
                      "window_s": emb.CLAP_WINDOW_S, **spec,
                      "dim": int(X.shape[1])},
        "training": {"corpora": {"archive_org": len(labels_a), **extra_counts},
                     "tracks": len(labels), "genres": len(gen_classes),
                     "families": dict(sorted(Counter(fam).items())),
                     "labels": "archive.org tags and FMA artist tags (noisy)",
                     "class_weight": args.class_weight,
                     "extras": extra_setup,
                     "cv": "stratified 4-fold over the archive.org corpus; extra corpora train only"},
        "cv_accuracy": {"family_top1": round(best["family"][1], 4), "genre_top1": round(best["genre"][1], 4),
                        "genre_top5": round(best["genre"][2], 4), "group_top1": round(g1, 4),
                        "group_top5": round(g5, 4), "majority_family_baseline": round(majority, 4),
                        "tracks": int(scored.sum())},
        "genre_parent": {g: parent[g] for g in gen_classes},
        "genre_groups": groups,
        "family": _head_json(fam_model),
        "genre": _head_json(gen_model),
    }
    args.out.write_text(json.dumps(probe, separators=(",", ":")) + "\n", encoding="utf-8")
    print(f"wrote {args.out} ({args.out.stat().st_size / 1024:.0f} KB): family {best['family'][1]:.1%}, "
          f"genre {best['genre'][1]:.1%} / top-5 {best['genre'][2]:.1%}, group {g1:.1%}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
