"""Extract only the MTG-Jamendo audio tracks that carry a mood/theme tag from
the downloaded shard tarballs, so mood_tagger.py can train on the full tag
overlap instead of the small 10-shard sample used previously.

Reads corpus_data/mtg_jamendo_tags/autotagging_moodtheme.tsv for the set of
tagged track IDs, then streams each data/train/N.tar shard and extracts only
members whose numeric ID is in that set (avoids extracting the full ~99GB
dataset when only the tagged subset is needed for training).

    python scripts/extract_jamendo_mood_tracks.py \
        --shards-dir ../corpus_data/mtg_jamendo_real/data/train \
        --tags-tsv ../corpus_data/mtg_jamendo_tags/autotagging_moodtheme.tsv \
        --out-dir ../corpus_data/mtg_jamendo_real/extracted
"""
from __future__ import annotations

import argparse
import csv
import glob
import os
import tarfile


def _tagged_track_ids(tags_tsv: str) -> set[str]:
    ids: set[str] = set()
    with open(tags_tsv, encoding="utf-8") as f:
        reader = csv.DictReader(f, delimiter="\t")
        for row in reader:
            ids.add(str(int(row["TRACK_ID"].split("_")[1])))
    return ids


def extract(shards_dir: str, tags_tsv: str, out_dir: str) -> dict:
    wanted = _tagged_track_ids(tags_tsv)
    os.makedirs(out_dir, exist_ok=True)
    already = {os.path.basename(f)[:-5] for f in glob.glob(os.path.join(out_dir, "*.opus"))}
    remaining = wanted - already

    shard_paths = sorted(glob.glob(os.path.join(shards_dir, "*.tar")),
                         key=lambda p: int(os.path.basename(p)[:-4]))
    n_extracted = 0
    for i, shard_path in enumerate(shard_paths):
        if not remaining:
            break
        with tarfile.open(shard_path, "r") as tf:
            for member in tf:
                if not member.isfile() or not member.name.endswith(".opus"):
                    continue
                tid = member.name[:-5]
                if tid not in remaining:
                    continue
                data = tf.extractfile(member)
                if data is None:
                    continue
                with open(os.path.join(out_dir, member.name), "wb") as out_f:
                    out_f.write(data.read())
                remaining.discard(tid)
                n_extracted += 1
        print(f"shard {i + 1}/{len(shard_paths)} ({os.path.basename(shard_path)}): "
              f"{n_extracted} extracted so far, {len(remaining)} still wanted", flush=True)

    return {"wanted": len(wanted), "already_had": len(already),
             "newly_extracted": n_extracted, "still_missing": len(remaining)}


if __name__ == "__main__":   # pragma: no cover
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--shards-dir", default="../corpus_data/mtg_jamendo_real/data/train")
    ap.add_argument("--tags-tsv", default="../corpus_data/mtg_jamendo_tags/autotagging_moodtheme.tsv")
    ap.add_argument("--out-dir", default="../corpus_data/mtg_jamendo_real/extracted")
    args = ap.parse_args()
    result = extract(args.shards_dir, args.tags_tsv, args.out_dir)
    print(result)
