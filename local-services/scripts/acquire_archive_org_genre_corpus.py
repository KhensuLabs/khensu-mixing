"""Acquire a CC-licensed, genre/subgenre-tagged audio corpus from archive.org
for the (currently blocked-on-licensed-data) governed genre classifier path -
see TODO.md and docs/evaluation/corpus-and-intervention-study.md.

Complements corpus_data/mtg_jamendo_real/: MTG-Jamendo's tag vocabulary only
overlaps ~23/139 of Khensu's genre_targets.json subgenres (verified
2026-07-17 - Jamendo's academic snapshot has 94 broad genre tags total, no
equivalents for genres like "amapiano" or "dungeon synth"). Archive.org's
netlabel-era catalog has real `subject`-tagged, CC-licensed audio across a
much wider slice of niche electronic/alternative genres.

For each genre in resources/genre_norms/genre_targets.json:
  1. Search archive.org (subject-field match, mediatype:audio,
     licenseurl:*creativecommons*).
  2. Reject "-nd" (No Derivatives) licenses - training a classifier is
     arguably a derivative use, so only by/by-sa/by-nc/by-nc-sa/cc0/
     publicdomain items are kept.
  3. Download up to --per-genre tracks (preferring compressed formats -
     MP3/OGG over FLAC/WAV - to bound disk/bandwidth), skipping items
     without a suitable file.
  4. Write a manifest (genre -> track -> archive.org identifier + license +
     download URL) so provenance travels with the audio, matching
     corpus_schema.py's "every item carries documented origin + license"
     requirement.

This does NOT train a genre classifier - the classifier itself stays gated
behind the full expert-review contract regardless of how much raw audio
exists. This only unblocks the audio-acquisition half of that gate.

    python scripts/acquire_archive_org_genre_corpus.py --per-genre 10
"""
from __future__ import annotations

import argparse
import json
import os
import time
import urllib.parse
import urllib.request

_REJECTED_LICENSE_SUBSTRINGS = ("-nd/", "-nd-")
_PREFERRED_FORMATS = ("VBR MP3", "MP3", "Ogg Vorbis", "128Kbps MP3")
_MAX_FILE_MB = 20


def _archive_get(url: str, timeout: int = 20) -> dict:
    with urllib.request.urlopen(url, timeout=timeout) as resp:
        return json.loads(resp.read())


def _search_genre(genre_id: str, rows: int = 40) -> list[dict]:
    q = genre_id.replace("-", " ")
    query = f'subject:"{q}" AND mediatype:audio AND licenseurl:*creativecommons*'
    params = [("q", query), ("rows", str(rows)), ("output", "json"),
              ("fl[]", "identifier"), ("fl[]", "licenseurl")]
    url = "https://archive.org/advancedsearch.php?" + urllib.parse.urlencode(params)
    data = _archive_get(url)
    return data.get("response", {}).get("docs", [])


def _is_permissive_license(licenseurl: str | None) -> bool:
    if not licenseurl:
        return False
    lic = licenseurl.lower()
    if "creativecommons.org" not in lic and "publicdomain" not in lic:
        return False
    return not any(bad in lic for bad in _REJECTED_LICENSE_SUBSTRINGS)


def _pick_audio_file(files: list[dict]) -> dict | None:
    by_format = {f.get("format"): f for f in files}
    for fmt in _PREFERRED_FORMATS:
        f = by_format.get(fmt)
        if f and int(f.get("size", 0)) < _MAX_FILE_MB * 1024 * 1024:
            return f
    # Fall back to the smallest audio-looking file under the size cap.
    candidates = [f for f in files
                 if f.get("format", "").lower() in ("vbr mp3", "mp3", "ogg vorbis")
                 and int(f.get("size", 0)) < _MAX_FILE_MB * 1024 * 1024]
    return min(candidates, key=lambda f: int(f.get("size", 0))) if candidates else None


def acquire(out_dir: str, per_genre: int, genre_targets_path: str,
           sleep_sec: float = 0.3) -> dict:
    genres = json.load(open(genre_targets_path, encoding="utf-8"))["genres"]
    manifest: dict[str, list[dict]] = {}
    os.makedirs(out_dir, exist_ok=True)

    for g in genres:
        gid = g["id"]
        genre_dir = os.path.join(out_dir, gid)
        entries: list[dict] = []
        try:
            docs = _search_genre(gid, rows=per_genre * 4)
        except Exception as exc:
            manifest[gid] = [{"error": str(exc)}]
            continue
        time.sleep(sleep_sec)

        for doc in docs:
            if len(entries) >= per_genre:
                break
            identifier = doc.get("identifier")
            licenseurl = doc.get("licenseurl")
            if not identifier or not _is_permissive_license(licenseurl):
                continue
            try:
                meta = _archive_get(f"https://archive.org/metadata/{identifier}", timeout=15)
            except Exception:
                continue
            time.sleep(sleep_sec)
            f = _pick_audio_file(meta.get("files", []))
            if f is None:
                continue

            os.makedirs(genre_dir, exist_ok=True)
            fname = f["name"]
            base = os.path.basename(fname)
            # Some archive.org uploads have no extension even when the
            # `format` field confirms MP3 - append one so downstream tools
            # (librosa/ffmpeg format sniffing) don't need to guess.
            if "." not in base and f.get("format", "").lower() in ("vbr mp3", "mp3", "128kbps mp3"):
                base += ".mp3"
            dest = os.path.join(genre_dir, f"{identifier}__{base}")
            url = f"https://archive.org/download/{identifier}/{urllib.parse.quote(fname)}"
            if not os.path.exists(dest):
                try:
                    urllib.request.urlretrieve(url, dest)
                except Exception:
                    continue
                time.sleep(sleep_sec)
            entries.append({
                "identifier": identifier, "file": os.path.basename(dest),
                "license": licenseurl, "source_url": f"https://archive.org/details/{identifier}",
            })

        manifest[gid] = entries
        covered = sum(1 for v in manifest.values() if len(v) >= per_genre // 2)
        print(f"{gid}: {len(entries)}/{per_genre}  (genres with a usable sample so far: {covered})", flush=True)

    manifest_path = os.path.join(out_dir, "manifest.json")
    with open(manifest_path, "w", encoding="utf-8") as fh:
        json.dump(manifest, fh, indent=2)

    n_with_min = sum(1 for v in manifest.values() if len(v) >= max(1, per_genre // 2))
    return {
        "manifest_path": manifest_path,
        "genres_total": len(genres),
        "genres_with_any_track": sum(1 for v in manifest.values() if len(v) > 0),
        "genres_with_usable_sample": n_with_min,
        "total_tracks": sum(len(v) for v in manifest.values()),
    }


if __name__ == "__main__":   # pragma: no cover
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out-dir", default="../corpus_data/genre_corpus_archive_org")
    ap.add_argument("--per-genre", type=int, default=10)
    ap.add_argument("--genre-targets", default="../resources/genre_norms/genre_targets.json")
    args = ap.parse_args()
    result = acquire(args.out_dir, args.per_genre, args.genre_targets)
    print(json.dumps(result, indent=2))
