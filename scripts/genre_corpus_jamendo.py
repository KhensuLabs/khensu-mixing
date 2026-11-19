"""List the MTG-Jamendo tracks the genre probe can learn from (GEN-2).

MTG-Jamendo (Bogdanov et al. 2019; Creative-Commons tracks) tags 55,215
tracks with 95 genre tags (``data/autotagging_genre.tsv`` from
https://github.com/MTG/mtg-jamendo-dataset). Its vocabulary has no modern
hip-hop or internet subgenres (no trap, drill, hyperpop, phonk...), so it
can only add examples of genres FMA and the archive corpus already have.

This maps the tags that name a catalog genre, gives each track its most
specific mapped tag (the rarest), keeps tracks whose audio is already on
disk (``corpus_data/mtg_jamendo_real/extracted/<id>.opus``, the mood-tagger
subset) and caps every genre; writes ``genre<TAB>path`` lines for
``scripts/train_genre_probe.py --extra``.

    local-services/.venv/Scripts/python scripts/genre_corpus_jamendo.py \
        --tags autotagging_genre.tsv --out build/genre_probe/jamendo_items.tsv
"""
from __future__ import annotations

import argparse
import json
import random
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CATALOG = ROOT / "resources" / "genre_norms" / "genre_targets.json"
AUDIO = ROOT / "corpus_data" / "mtg_jamendo_real" / "extracted"

# MTG-Jamendo genre tag -> catalog genre id. Only tags that mean the same thing.
JAMENDO_TO_CATALOG = {
    "rock": "rock", "pop": "pop", "metal": "metal", "heavymetal": "metal", "punkrock": "punk",
    "postrock": "post-rock", "grunge": "grunge", "alternativerock": "alt-rock", "poprock": "pop-rock",
    "psychedelic": "psychedelic-rock", "gothic": "gothic-rock", "singersongwriter": "singer-songwriter",
    "hiphop": "rap", "rap": "rap", "rnb": "r-and-b", "soul": "soul", "funk": "funk", "disco": "disco",
    "blues": "blues", "jazz": "jazz", "country": "country", "reggae": "reggae",
    "ambient": "ambient", "darkambient": "dark-ambient", "techno": "techno", "minimal": "minimal-techno",
    "house": "house", "deephouse": "deep-house", "trance": "trance", "dubstep": "dubstep",
    "drumnbass": "drum-and-bass", "breakbeat": "breaks", "industrial": "industrial",
    "synthpop": "synthpop", "electropop": "electropop",
}


def build(tags_tsv: Path, cap: int, seed: int) -> list[tuple[str, Path]]:
    catalog = {g["id"] for g in json.loads(CATALOG.read_text(encoding="utf-8"))["genres"]}
    bad = sorted(set(JAMENDO_TO_CATALOG.values()) - catalog)
    if bad:
        raise SystemExit(f"mapping names genres missing from the catalog: {bad}")
    rows = [ln.split("\t") for ln in tags_tsv.read_text(encoding="utf-8").splitlines()[1:] if ln.strip()]
    freq = Counter(t[8:] for r in rows for t in r[5:] if t.startswith("genre---"))
    by_genre: dict[str, list[Path]] = {}
    for r in rows:
        tags = [t[8:] for t in r[5:] if t.startswith("genre---") and t[8:] in JAMENDO_TO_CATALOG]
        if not tags:
            continue
        path = AUDIO / f"{int(r[0].split('_')[1])}.opus"
        if not path.exists():
            continue
        tag = min(tags, key=lambda t: (freq[t], t))
        by_genre.setdefault(JAMENDO_TO_CATALOG[tag], []).append(path)
    rng = random.Random(seed)
    items = []
    for gid, paths in sorted(by_genre.items()):
        rng.shuffle(paths)
        items += [(gid, p) for p in sorted(paths[:cap])]
    return items


def main() -> int:
    ap = argparse.ArgumentParser(description="List the MTG-Jamendo tracks the genre probe can learn from.")
    ap.add_argument("--tags", type=Path, required=True, help="MTG-Jamendo data/autotagging_genre.tsv")
    ap.add_argument("--cap", type=int, default=300, help="most tracks kept per genre")
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--out", type=Path, default=ROOT / "build" / "genre_probe" / "jamendo_items.tsv")
    args = ap.parse_args()
    items = build(args.tags, args.cap, args.seed)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text("".join(f"{g}\t{p}\n" for g, p in items), encoding="utf-8")
    counts = Counter(g for g, _ in items)
    print(f"{len(items)} tracks in {len(counts)} genres -> {args.out}")
    for g, n in counts.most_common():
        print(f"  {g:20s} {n}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
