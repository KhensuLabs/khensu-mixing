# local-services/khensu_services/util/naming.py
"""Shared output-naming conventions (spec §1.3).

Stem-separation folders use AMAS_<M>_<D>_<YYYY>_<SongTitle>, e.g.
AMAS_7_10_2026_Shadowinmyroom — month/day unpadded, title taken from the
source filename stem with whitespace and non [A-Za-z0-9_-] characters
stripped. When that stripping drops anything other than spaces (accents,
non-Latin scripts, punctuation), the title gets ``_<8 hex>`` appended: FNV-1a-32 over the
UTF-8 filename stem as 8 lowercase hex digits (trivially mirrored in C++), so two different
non-Latin names in one folder no longer collapse onto the same
"Untitled"/"Trck" folder and serve each other's stems (AUDIT-074). The C++
desktop (AppController::getStemsDirectory) mirrors this format; change them
together.
"""
from __future__ import annotations

import datetime as _dt
import os
import re

_SAFE = re.compile(r"[^A-Za-z0-9_-]+")


def sanitize_title(name: str) -> str:
    """Filename stem → folder-safe title: whitespace and special characters
    removed, original casing preserved."""
    cleaned = _SAFE.sub("", name.replace(" ", ""))
    return cleaned or "Untitled"


def _stem_title(stem: str) -> str:
    """``sanitize_title`` plus a disambiguating hash when the sanitisation was
    lossy beyond whitespace (see module doc)."""
    title = sanitize_title(stem)
    if title != stem.replace(" ", ""):
        title += "_" + _fnv1a32_hex(stem)
    return title


def _fnv1a32_hex(text: str) -> str:
    h = 0x811C9DC5
    for byte in text.encode("utf-8", "surrogatepass"):
        h = ((h ^ byte) * 0x01000193) & 0xFFFFFFFF
    return f"{h:08x}"


def _basename(song_path: str) -> str:
    """Cross-platform basename: split on BOTH separators regardless of host OS.

    Windows is the primary target, so Windows-style paths (``C:\\music\\x.wav``)
    reach this code even when the service is exercised on Linux (CI). Plain
    ``os.path.basename`` only honours the host separator, so on Linux the
    backslashes survive and the drive/folder prefix leaks into the title.
    """
    return re.split(r"[\\/]", song_path)[-1]


def amas_folder_name(song_path: str, when: _dt.date | None = None) -> str:
    when = when or _dt.date.today()
    title = _stem_title(os.path.splitext(_basename(song_path))[0])
    return f"AMAS_{when.month}_{when.day}_{when.year}_{title}"


def amas_stem_dir(song_path: str, when: _dt.date | None = None) -> str:
    """Default stem-output directory: an AMAS-named folder next to the song.

    If an AMAS folder for the same title already exists beside the song
    (from an earlier day's run), it is reused so stems act as a cache
    instead of accumulating one folder per day.
    """
    parent = os.path.dirname(os.path.abspath(song_path))
    title = _stem_title(os.path.splitext(_basename(song_path))[0])
    suffix = "_" + title
    try:
        existing = sorted(
            (d for d in os.listdir(parent)
             if d.startswith("AMAS_") and d.endswith(suffix)
             and os.path.isdir(os.path.join(parent, d))),
            key=lambda d: os.path.getmtime(os.path.join(parent, d)),
            reverse=True,
        )
    except OSError:
        existing = []
    if existing:
        return os.path.join(parent, existing[0])
    return os.path.join(parent, amas_folder_name(song_path, when))
