"""Fetch a per-generator subset of FakeMusicCaps without the whole 12.9 GB zip.

FakeMusicCaps (Comanducci, Bestagini, Tubaro; Zenodo 15063698, CC BY-NC 4.0):
10 s clips from five text-to-music models. Zenodo serves HTTP range requests,
and a zip's directory sits at its end, so the archive is opened remotely and
only the chosen clips are read.

    local-services/.venv/Scripts/python scripts/fetch_fakemusiccaps_subset.py [--per-generator 120]

Clips land in corpus_data/fakemusiccaps/<generator>/ (git-ignored).
"""
from __future__ import annotations

import argparse
import http.client
import io
import random
import urllib.request
import zipfile
from pathlib import Path, PurePosixPath

ROOT = Path(__file__).resolve().parents[1]
URL = "https://zenodo.org/api/records/15063698/files/FakeMusicCaps.zip/content"
OUT = ROOT / "corpus_data" / "fakemusiccaps"


class HttpRangeFile(io.RawIOBase):
    """A read-only, seekable view of a remote file through HTTP range requests."""

    def __init__(self, url: str) -> None:
        self.url = url
        req = urllib.request.Request(url, method="HEAD")
        with urllib.request.urlopen(req, timeout=60) as r:
            self.size = int(r.headers["Content-Length"])
        self.pos = 0

    def seekable(self) -> bool:
        return True

    def readable(self) -> bool:
        return True

    def tell(self) -> int:
        return self.pos

    def seek(self, offset: int, whence: int = io.SEEK_SET) -> int:
        base = {io.SEEK_SET: 0, io.SEEK_CUR: self.pos, io.SEEK_END: self.size}[whence]
        self.pos = max(0, base + offset)
        return self.pos

    def readinto(self, b) -> int:
        if self.pos >= self.size:
            return 0
        end = min(self.size, self.pos + len(b)) - 1
        req = urllib.request.Request(self.url, headers={"Range": f"bytes={self.pos}-{end}"})
        data = b""
        for attempt in range(5):
            try:
                with urllib.request.urlopen(req, timeout=120) as r:
                    data = r.read()
                break
            except (OSError, http.client.HTTPException):  # a dropped connection, an incomplete read
                if attempt == 4:
                    raise
        n = len(data)
        b[:n] = data
        self.pos += n
        return n


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--per-generator", type=int, default=120)
    a = ap.parse_args()
    remote = io.BufferedReader(HttpRangeFile(URL), buffer_size=4 * 1024 * 1024)
    with zipfile.ZipFile(remote) as zf:
        by_gen: dict[str, list[zipfile.ZipInfo]] = {}
        for info in zf.infolist():
            p = PurePosixPath(info.filename)
            # Skip macOS AppleDouble metadata (__MACOSX/, ._name.wav): not audio.
            if (info.is_dir() or p.suffix.lower() != ".wav" or len(p.parts) < 2
                    or p.name.startswith("._") or "__MACOSX" in p.parts):
                continue
            by_gen.setdefault(p.parts[-2], []).append(info)
        print({g: len(v) for g, v in sorted(by_gen.items())}, flush=True)
        rng = random.Random(0)
        for gen, infos in sorted(by_gen.items()):
            infos.sort(key=lambda i: i.filename)
            rng.shuffle(infos)
            dest = OUT / gen
            dest.mkdir(parents=True, exist_ok=True)
            got = 0
            for info in infos[: a.per_generator]:
                # Only the file name is used: the archive's own paths never decide where we write.
                target = dest / PurePosixPath(info.filename).name
                if not target.exists():
                    target.write_bytes(zf.read(info))
                got += 1
            print(f"{gen}: {got} clips", flush=True)


if __name__ == "__main__":
    main()
