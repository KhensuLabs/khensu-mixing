"""Fetch lossless originals for part of the genre corpus (MEAS-4).

The genre corpus (corpus_data/genre_corpus_archive_org) is MP3 and Ogg. A
lossy decode overshoots its master's peaks, so true-peak and clipping
findings cannot be calibrated on it: they would measure the codec. Many
archive.org netlabel items also carry the FLAC or WAV the MP3 was made from;
this fetches, for up to PER_GENRE tracks of each genre, the lossless file
whose name matches the corpus track, into corpus_data/genre_corpus_lossless/
with a manifest beside it. Only public item identifiers are sent.

    python scripts/fetch_lossless_subset.py [--per-genre 3] [--max-mb 150]
"""
from __future__ import annotations

import argparse
import json
import time
import urllib.parse
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "corpus_data" / "genre_corpus_archive_org"
DST = ROOT / "corpus_data" / "genre_corpus_lossless"
UA = {"User-Agent": "corpus-fetch/1.0"}
LOSSLESS = {"flac", "24bit flac", "wave", "aiff"}


def _get_json(url: str) -> dict:
    with urllib.request.urlopen(urllib.request.Request(url, headers=UA), timeout=60) as r:
        return json.loads(r.read().decode("utf-8"))


def _stem(name: str) -> str:
    return name.rsplit(".", 1)[0].strip().lower()


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--per-genre", type=int, default=3)
    ap.add_argument("--max-mb", type=float, default=150.0)
    args = ap.parse_args()
    manifest = json.loads((SRC / "manifest.json").read_text(encoding="utf-8"))
    out_manifest_path = DST / "manifest.json"
    out: dict[str, list[dict]] = (json.loads(out_manifest_path.read_text(encoding="utf-8"))
                                  if out_manifest_path.exists() else {})
    meta_cache: dict[str, dict] = {}
    for genre, tracks in manifest.items():
        have = out.setdefault(genre, [])
        tried = {t["lossy_file"] for t in have}
        for t in tracks:
            if len([x for x in have if x.get("file")]) >= args.per_genre:
                break
            if t["file"] in tried:
                continue
            ident = t["identifier"]
            original = t["file"].split("__", 1)[-1]
            entry = {"identifier": ident, "lossy_file": t["file"], "license": t.get("license"),
                     "source_url": t.get("source_url")}
            try:
                meta = meta_cache.get(ident) or _get_json(f"https://archive.org/metadata/{urllib.parse.quote(ident)}")
                meta_cache[ident] = meta
            except Exception as exc:  # noqa: BLE001 - a failed item is skipped and recorded
                entry["error"] = f"metadata: {exc}"
                have.append(entry)
                continue
            match = None
            for f in meta.get("files", []):
                if str(f.get("format", "")).lower() in LOSSLESS and _stem(Path(f["name"]).name) == _stem(Path(original).name):
                    match = f
                    break
            if match is None:
                entry["error"] = "no lossless original"
                have.append(entry)
                continue
            size_mb = float(match.get("size", 0)) / 1e6
            if size_mb > args.max_mb:
                entry["error"] = f"too large ({size_mb:.0f} MB)"
                have.append(entry)
                continue
            ext = Path(match["name"]).suffix.lower()
            dest = DST / genre / f"{ident}__{Path(original).stem}{ext}"
            dest.parent.mkdir(parents=True, exist_ok=True)
            url = f"https://archive.org/download/{urllib.parse.quote(ident)}/{urllib.parse.quote(match['name'])}"
            try:
                with urllib.request.urlopen(urllib.request.Request(url, headers=UA), timeout=300) as r, \
                        open(dest, "wb") as fh:
                    while chunk := r.read(1 << 20):
                        fh.write(chunk)
            except Exception as exc:  # noqa: BLE001
                dest.unlink(missing_ok=True)
                entry["error"] = f"download: {exc}"
                have.append(entry)
                continue
            entry["file"] = dest.name
            entry["format"] = match.get("format")
            have.append(entry)
            print(f"{genre}: {dest.name!a} ({size_mb:.0f} MB)", flush=True)
            out_manifest_path.parent.mkdir(parents=True, exist_ok=True)
            out_manifest_path.write_text(json.dumps(out, indent=1), encoding="utf-8")
            time.sleep(0.5)
        out_manifest_path.parent.mkdir(parents=True, exist_ok=True)
        out_manifest_path.write_text(json.dumps(out, indent=1), encoding="utf-8")
    got = sum(1 for g in out.values() for x in g if x.get("file"))
    print(f"done: {got} lossless tracks")


if __name__ == "__main__":
    main()
