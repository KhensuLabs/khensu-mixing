"""EXP-1: does "Similar references" find similar music on a real library?

The reference index (analysis/embeddings.py) ranks tracks by cosine
similarity of a CLAP or hand-built embedding. This builds that index over the
genre corpus's lossless originals (fetch_lossless_subset.py) with the app's
own ``embed_file`` and asks, for every track left out in turn: is its nearest
neighbour from the same genre, or from the same genre family (the genre
table's ``parent``)? Neighbours from the same archive.org item (the same
album or artist - the key before ``__``) are skipped, so a hit is music that
is alike, not the next track of the same record. Chance is the same score
with the genre labels shuffled (mean of 200 shuffles).

    local-services/.venv/Scripts/python scripts/eval_similar_references.py [--backends clap,handcrafted]

Embeddings are cached in build/similar_refs/; the result goes to
bench-results/similar_references.json.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "local-services"))
LOSSLESS = ROOT / "corpus_data" / "genre_corpus_lossless"
CACHE = ROOT / "build" / "similar_refs"
OUT = ROOT / "bench-results" / "similar_references.json"


def tracks() -> list[tuple[str, str]]:
    manifest = json.loads((LOSSLESS / "manifest.json").read_text(encoding="utf-8"))
    return [(str(LOSSLESS / g / t["file"]), g) for g, ts in sorted(manifest.items()) for t in ts
            if t.get("file") and (LOSSLESS / g / t["file"]).is_file()]


def embed_all(backend: str, items: list[tuple[str, str]]) -> np.ndarray:
    from khensu_services.analysis.embeddings import embed_file

    path = CACHE / f"{backend}.npz"
    have: dict[str, np.ndarray] = {}
    if path.exists():
        d = np.load(path, allow_pickle=False)
        have = {str(k): v for k, v in zip(d["keys"], d["embs"])}
    for n, (p, _g) in enumerate(items, 1):
        if p not in have:
            have[p], _ = embed_file(p, backend)
            if n % 20 == 0:
                CACHE.mkdir(parents=True, exist_ok=True)
                np.savez(path, keys=np.array(list(have)), embs=np.stack(list(have.values())))
                print(f"{backend} {n}/{len(items)}", flush=True)
    CACHE.mkdir(parents=True, exist_ok=True)
    np.savez(path, keys=np.array(list(have)), embs=np.stack(list(have.values())))
    return np.stack([have[p] for p, _ in items])


def score(X: np.ndarray, labels: np.ndarray, groups: np.ndarray, k: int = 5) -> dict[str, float]:
    Xn = X / np.maximum(np.linalg.norm(X, axis=1, keepdims=True), 1e-12)
    sims = Xn @ Xn.T
    top1, topk = [], []
    for i in range(len(X)):
        order = [j for j in np.argsort(-sims[i]) if j != i and groups[j] != groups[i]]
        if not order:
            continue
        top1.append(labels[order[0]] == labels[i])
        topk.append(any(labels[j] == labels[i] for j in order[:k]))
    return {"p_at_1": float(np.mean(top1)), f"hit_at_{k}": float(np.mean(topk))}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--backends", default="clap,handcrafted")
    a = ap.parse_args()
    items = tracks()
    genres = json.loads((ROOT / "resources" / "genre_norms" / "genre_targets.json").read_text(encoding="utf-8"))
    parent = {e["id"]: (e.get("parent") or e["id"]) for e in genres["genres"]}
    genre = np.array([g for _, g in items])
    family = np.array([parent.get(g, g) for g in genre])
    group = np.array([Path(p).name.split("__", 1)[0] for p, _ in items])
    rng = np.random.default_rng(0)
    report: dict = {"tracks": len(items), "genres": len(set(genre)), "families": len(set(family)),
                    "excludes": "neighbours from the same archive.org item (album / artist)", "backends": {}}
    for backend in [b for b in a.backends.split(",") if b]:
        X = embed_all(backend, items)
        entry = {}
        for name, lab in (("genre", genre), ("family", family)):
            real = score(X, lab, group)
            chance = [score(X, rng.permutation(lab), group) for _ in range(200)]
            entry[name] = {**{m: round(v, 3) for m, v in real.items()},
                           **{f"chance_{m}": round(float(np.mean([c[m] for c in chance])), 3) for m in real}}
        report["backends"][backend] = entry
        print(backend, json.dumps(entry), flush=True)
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(json.dumps(report, indent=1), encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
