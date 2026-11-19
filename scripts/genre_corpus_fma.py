"""List the FMA tracks the genre probe can learn from (GEN-1b).

FMA (Free Music Archive, Defferrard et al. 2017; tracks Creative Commons,
metadata CC BY 4.0) tags each track with one or more of its 163 genres.
This maps the FMA genres that name a catalog genre onto it, gives each
track its most specific mapped tag (the one with the fewest tracks, so
"Rock" + "Indie-Rock" is indie-rock), caps every genre so rock does not
drown the rest, and writes ``genre<TAB>path`` lines for
``scripts/train_genre_probe.py --extra``.

    local-services/.venv/Scripts/python scripts/genre_corpus_fma.py \
        --fma D:/khensu-datasets/fma --out build/genre_probe/fma_items.tsv

FMA genres with no catalog counterpart (Experimental, Folk, Electronic,
Instrumental, ...) are left out rather than forced onto a near neighbour.
"""
from __future__ import annotations

import argparse
import ast
import csv
import json
import random
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CATALOG = ROOT / "resources" / "genre_norms" / "genre_targets.json"

# FMA genre title -> catalog genre id. Only names that mean the same thing.
FMA_TO_CATALOG = {
    "Blues": "blues", "Jazz": "jazz", "Free-Jazz": "free-jazz", "Country": "country",
    "Pop": "pop", "Power-Pop": "power-pop", "Synth Pop": "synthpop", "Disco": "disco",
    "Funk": "funk", "Soul-RnB": "r-and-b",
    "Rock": "rock", "Indie-Rock": "indie-rock", "Post-Rock": "post-rock", "Post-Punk": "post-punk",
    "Punk": "punk", "Hardcore": "hardcore-punk", "Garage": "garage-rock", "Noise-Rock": "noise-rock",
    "Psych-Rock": "psychedelic-rock", "Progressive": "prog-rock", "Shoegaze": "shoegaze",
    "Surf": "surf-rock", "Goth": "gothic-rock",
    "Metal": "metal", "Thrash": "metal", "Death-Metal": "death-metal", "Black-Metal": "black-metal",
    "Hip-Hop": "rap", "Rap": "rap", "Abstract Hip-Hop": "abstract-hip-hop",
    "Techno": "techno", "House": "house", "Dubstep": "dubstep", "Drum & Bass": "drum-and-bass",
    "Jungle": "jungle", "Breakbeat": "breaks", "Breakcore - Hard": "breakcore",
    "Industrial": "industrial", "Ambient": "ambient", "Ambient Electronic": "ambient", "Drone": "drone",
    "Reggae - Dub": "reggae", "Reggae - Dancehall": "dancehall",
    "Singer-Songwriter": "singer-songwriter",
}


def build(fma: Path, subset: str, cap: int, seed: int) -> list[tuple[str, Path]]:
    catalog = {g["id"] for g in json.loads(CATALOG.read_text(encoding="utf-8"))["genres"]}
    bad = sorted(set(FMA_TO_CATALOG.values()) - catalog)
    if bad:
        raise SystemExit(f"mapping names genres missing from the catalog: {bad}")
    meta = fma / "fma_metadata"
    with open(meta / "genres.csv", encoding="utf-8") as f:
        genres = {int(r["genre_id"]): (r["title"], int(r["#tracks"])) for r in csv.DictReader(f)}
    with open(meta / "tracks.csv", encoding="utf-8") as f:
        rows = csv.reader(f)
        h1, h2 = next(rows), next(rows)
        next(rows)  # the "track_id" row
        col = {(a, b): i for i, (a, b) in enumerate(zip(h1, h2))}
        i_subset, i_genres = col[("set", "subset")], col[("track", "genres")]
        audio = fma / f"fma_{subset}"
        order = ("small", "medium", "large")
        allowed = set(order[: order.index(subset) + 1])
        by_genre: dict[str, list[Path]] = {}
        for r in rows:
            if r[i_subset] not in allowed:
                continue
            tags = [genres[g] for g in ast.literal_eval(r[i_genres] or "[]") if g in genres]
            mapped = [(n, FMA_TO_CATALOG[t]) for t, n in tags if t in FMA_TO_CATALOG]
            if not mapped:
                continue
            gid = min(mapped)[1]  # the most specific (rarest) mapped tag
            tid = int(r[0])
            path = audio / f"{tid:06d}"[:3] / f"{tid:06d}.mp3"
            by_genre.setdefault(gid, []).append(path)
    rng = random.Random(seed)
    items = []
    for gid, paths in sorted(by_genre.items()):
        paths = [p for p in paths if p.exists()]
        rng.shuffle(paths)
        items += [(gid, p) for p in sorted(paths[:cap])]
    return items


def main() -> int:
    ap = argparse.ArgumentParser(description="List the FMA tracks the genre probe can learn from.")
    ap.add_argument("--fma", type=Path, required=True, help="folder holding fma_metadata/ and fma_<subset>/")
    ap.add_argument("--subset", default="medium", choices=("small", "medium", "large"))
    ap.add_argument("--cap", type=int, default=400, help="most tracks kept per genre")
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--out", type=Path, default=ROOT / "build" / "genre_probe" / "fma_items.tsv")
    args = ap.parse_args()
    items = build(args.fma, args.subset, args.cap, args.seed)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text("".join(f"{g}\t{p}\n" for g, p in items), encoding="utf-8")
    counts = Counter(g for g, _ in items)
    print(f"{len(items)} tracks in {len(counts)} genres -> {args.out}")
    for g, n in counts.most_common():
        print(f"  {g:20s} {n}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
