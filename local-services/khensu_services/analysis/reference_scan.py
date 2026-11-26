# local-services/khensu_services/analysis/reference_scan.py
"""The reference folder scan (REFS-1).

The user points the References screen at a folder - their music folder, a
sample library. This walks it, and for every audio file:

* sorts it into a **song** or a **sample** (``classify``): duration, the
  folder names on the way down ("Samples", "Loops", "Construction Kit",
  "One Shots", ...), the file name (a BPM, a key, "loop", "one shot", a drum
  hit) and whether it carries album tags. A sample is a ``loop`` or a
  ``one_shot``.
* reads its tags with mutagen (artist / album / title / genre / BPM / key,
  plus the WAV RIFF INFO chunk sample packs use) and its artwork: the
  embedded cover, else a cover / folder / artwork image in its folder or its
  pack's folder, cached as a 160 px JPEG under the config dir.
* finds a sample's **construction pack** - the ancestor folder that looks
  like a pack (a cover image, category subfolders like Drums / Loops / One
  Shots, "Pack" / "Kit" / "Vol" in the name) - and its **author**: the tag,
  else the label / vendor folder above the pack, else "Vendor - Pack".
* gives it a catalog **genre** (``resources/genre_norms/genre_targets.json``
  ids, with the parent family): the genre tag when it names a catalog genre,
  else a genre in the folder names, else - for songs and loops - the genre
  probe (``learned.genre_probe``, as Intake) or the hand-tuned ranker
  (``genre_detector.estimate_genre_blend``) on a 30 s excerpt, with a
  confidence. One-shots without a tag or folder genre take their pack's.

Rows live in khensu.db (``reference_scan_roots`` / ``reference_scan_items``,
migration 017). A rescan skips files whose size and mtime are unchanged and
forgets files that are gone. Songs are added to the reference library
(``analysis/embeddings`` index, FEAT-5) with their genre, so they show in the
genre's references and in Find similar.
"""
from __future__ import annotations

import base64
import hashlib
import io
import json
import logging
import os
import re
import stat as _stat
import struct
import time
from collections import Counter
from pathlib import Path
from typing import Any, Callable, Iterable

import numpy as np

_log = logging.getLogger(__name__)

AUDIO_EXTS = frozenset({".wav", ".wave", ".aif", ".aiff", ".aifc", ".flac", ".mp3", ".ogg", ".oga",
                        ".opus", ".m4a"})
IMAGE_EXTS = (".jpg", ".jpeg", ".png")
#: Image stems tried in order before "any single image in the folder".
COVER_STEMS = ("cover", "folder", "front", "artwork", "art", "album", "albumart", "albumartsmall", "pack")
THUMB_PX = 160
EXCERPT_S = 30.0
DEFAULT_MAX_FILES = 50000
#: Directories never entered (besides hidden / system ones).
SKIP_DIRS = frozenset({"$recycle.bin", "system volume information", "__macosx", "node_modules", ".git",
                       "appdata", "windows", "program files", "program files (x86)", "temp"})
#: Category folder names inside sample packs - never a pack or author name.
GENERIC_DIRS = frozenset(x.strip() for x in """
drums|drum|loops|loop|one shots|one shot|oneshots|oneshot|shots|kicks|kick|snares|snare|hats|hat|hihats|
hi hats|hihat|claps|clap|percussion|perc|percs|fx|sfx|vocals|vocal|vox|bass|basses|synths|synth|keys|pads|
pad|leads|lead|melodies|melody|music loops|drum loops|samples|sample|sample packs|packs|kits|
construction kits|construction kit|stems|wav|wavs|audio|midi|presets|808s|808|toms|cymbals|rides|crashes|
shakers|risers|impacts|textures|atmospheres|atmos|guitars|guitar|piano|strings|brass|chords|top loops|tops|
full loops|sounds|sound|instruments|instrument|bass loops|synth loops|vocal loops|one shot samples|
drum hits|hits|breaks|drum breaks|fills|foley|ambience|misc|extras|bonus|24bit|16bit|24 bit|16 bit|wet|
dry|processed|unprocessed|dry stems|wet stems|music|songs|downloads|my music|itunes|itunes media|
media|library|albums|unknown album|unknown artist""".replace("\n", "").split("|"))
#: Folder words that mean "sample library" anywhere above a file.
SAMPLE_FOLDER_WORDS = ("sample", "samples", "loops", "loop", "one shot", "one shots", "oneshot", "oneshots",
                       "kit", "kits", "construction", "drum kit", "sample pack", "splice", "stems", "fx",
                       "sfx", "drums", "percussion", "808", "foley", "presets", "midi")
MUSIC_FOLDER_WORDS = ("music", "albums", "itunes", "songs", "playlists", "discography")
#: Drum-hit and one-shot words in file names.
HIT_WORDS = ("kick", "snare", "hat", "hihat", "clap", "rim", "tom", "crash", "ride", "cymbal", "shaker",
             "perc", "808", "snap", "impact", "riser", "sweep", "fx", "stab", "hit", "chord shot")
#: Catalog keys that are too ambiguous to read out of a folder name.
FOLDER_GENRE_STOP = frozenset({"jerk", "rage", "breaks", "drone", "acoustic", "industrial", "plugg",
                               "score", "dance"})
#: Spellings of catalog genres / families that are not their names.
GENRE_ALIASES = {
    "hip hop": "hip-hop", "hiphop": "hip-hop", "hip hop rap": "hip-hop", "rap hip hop": "hip-hop",
    "rnb": "r-and-b", "r n b": "r-and-b", "rhythm and blues": "r-and-b", "r and b": "r-and-b",
    "dnb": "drum-and-bass", "d and b": "drum-and-bass", "drum n bass": "drum-and-bass",
    "drum bass": "drum-and-bass", "drum and bass": "drum-and-bass", "drumnbass": "drum-and-bass",
    "edm": "electronic", "electronica": "electronic", "electronic": "electronic", "dance": "electronic",
    "electro": "electronic", "lofi": "lo-fi-hip-hop", "lo fi": "lo-fi-hip-hop", "lo fi hip hop": "lo-fi-hip-hop",
    "lofi hip hop": "lo-fi-hip-hop", "chillhop": "lo-fi-hip-hop", "dubstep": "dubstep", "folk": "folk",
    "world": "world", "afrobeats": "afro-pop", "afrobeat": "afro-pop", "reggaeton": "latin-pop",
    "soundtrack": "cinematic", "score": "cinematic", "film score": "cinematic", "classical": "cinematic",
    "alternative": "alt-rock", "alternative rock": "alt-rock", "alt rock": "alt-rock", "indie": "indie-rock",
    "ukg": "uk-garage", "2 step": "uk-garage", "2step": "uk-garage", "dancehall": "reggae",
    "trap music": "trap", "deep tech": "tech-house", "psytrance": "trance", "psy trance": "trance",
    "boombap": "boom-bap", "boom bap": "boom-bap", "rhythm n blues": "r-and-b", "neo soul": "neo-soul",
}

_TRACKNO = re.compile(r"^\s*(?:\d{1,2}[-.]?\d{0,2}\s*[-._)]\s*|\d{1,3}\s+)")
_BPM = re.compile(r"(?<!\d)(\d{2,3})\s*-?\s*bpm(?![a-z])", re.IGNORECASE)
_KEY = re.compile(r"(?:^|[\s_\-(\[])([A-G](?:#|b|sharp|flat)?)\s?(min|minor|maj|major|m)(?=$|[\s_\-)\].])")
_KEY_BARE = re.compile(r"(?:^|[\s_\-(\[])([A-G](?:#|b))(?=$|[\s_\-)\].])")


# --- small helpers ------------------------------------------------------------

def norm(s: str) -> str:
    """Lower-case words: 'Drum & Bass' -> 'drum and bass', 'Hip-Hop' -> 'hip hop'."""
    s = str(s).lower().replace("&", " and ").replace("'", "")
    return " ".join(re.sub(r"[^a-z0-9#]+", " ", s).split())


def item_id(path: str) -> str:
    """The embeddings index id of ``path`` (one id for a file everywhere)."""
    from khensu_services.analysis.embeddings import entry_id
    return entry_id(path)


def utc_now() -> str:
    from khensu_services.collab.db import utc_now as _now
    return _now()


def thumb_dir() -> Path:
    from khensu_services.common.config import config_dir
    return config_dir() / "cache" / "reference_thumbs"


# --- the genre catalog -----------------------------------------------------------

_catalog_cache: dict[str, Any] | None = None


def catalog() -> dict[str, Any]:
    """{"genres": {id: (name, family)}, "families": set, "keys": [(norm key, id)] longest first}."""
    global _catalog_cache
    if _catalog_cache is not None:
        return _catalog_cache
    from khensu_services.analysis.learned.genre_probe import CATALOG_PATH
    genres: dict[str, tuple[str, str]] = {}
    try:
        for g in json.loads(CATALOG_PATH.read_text(encoding="utf-8")).get("genres", []):
            gid = str(g.get("id", ""))
            if gid:
                genres[gid] = (str(g.get("name", gid)), str(g.get("parent", gid)))
    except (OSError, ValueError):
        _log.warning("genre_targets.json unreadable; the reference scan sorts by tags only", exc_info=True)
    families = {fam for _, fam in genres.values()}
    keys: dict[str, str] = {}
    for gid, (name, _fam) in genres.items():
        keys.setdefault(norm(name), gid)
        keys.setdefault(norm(gid), gid)
    for fam in families:
        keys.setdefault(norm(fam), fam)
    for alias, target in GENRE_ALIASES.items():
        if target in genres or target in families:
            keys.setdefault(norm(alias), target)
    ordered = sorted(keys.items(), key=lambda kv: (-len(kv[0]), kv[0]))
    _catalog_cache = {"genres": genres, "families": families, "keys": ordered}
    return _catalog_cache


def resolve_genre(value: str) -> tuple[str, str]:
    """(catalog genre id or '', family) of a catalog id or family id."""
    cat = catalog()
    if value in cat["genres"]:
        return value, cat["genres"][value][1]
    if value in cat["families"]:
        return "", value
    return "", ""


def match_genre_text(text: str, *, stop: frozenset[str] = frozenset()) -> tuple[str, str, bool]:
    """The catalog genre a piece of text names: (genre id or '', family, exact).

    A whole-text match wins; then the longest catalog name found as whole
    words. A catalog genre beats a family-only match of the same length."""
    t = norm(text)
    if not t:
        return "", "", False
    cat = catalog()
    keys = cat["keys"]
    for key, target in keys:
        if key == t and key not in stop:
            g, fam = resolve_genre(target)
            return g, fam, True
    padded = f" {t} "
    best: tuple[str, str] | None = None
    for key, target in keys:
        if key in stop or len(key) < 3:
            continue
        if f" {key} " in padded:
            g, fam = resolve_genre(target)
            if g:
                return g, fam, False
            if best is None:
                best = (g, fam)
    return (best[0], best[1], False) if best else ("", "", False)


# --- tags -----------------------------------------------------------------------

def _first(value: Any) -> str:
    if value is None:
        return ""
    if isinstance(value, (list, tuple)):
        value = value[0] if value else ""
    text = getattr(value, "text", None)
    if text is not None:
        return _first(text)
    if isinstance(value, tuple):
        return str(value[0])
    return str(value).strip()


def _riff_info(path: str) -> dict[str, str]:
    """The LIST/INFO chunk of a WAV file (IART, INAM, IGNR, IPRD, ICMT, ...),
    which sample packs write instead of ID3."""
    out: dict[str, str] = {}
    try:
        with open(path, "rb") as fh:
            head = fh.read(12)
            if len(head) < 12 or head[:4] not in (b"RIFF", b"RF64") or head[8:12] != b"WAVE":
                return out
            for _ in range(64):
                hdr = fh.read(8)
                if len(hdr) < 8:
                    break
                cid, size = hdr[:4], struct.unpack("<I", hdr[4:])[0]
                if cid == b"LIST" and size <= 1 << 20:
                    body = fh.read(size)
                    if body[:4] == b"INFO":
                        pos = 4
                        while pos + 8 <= len(body):
                            sid, ssize = body[pos:pos + 4], struct.unpack("<I", body[pos + 4:pos + 8])[0]
                            raw = body[pos + 8:pos + 8 + ssize].split(b"\x00", 1)[0]
                            out[sid.decode("ascii", "replace")] = raw.decode("utf-8", "replace").strip()
                            pos += 8 + ssize + (ssize & 1)
                    if size & 1:
                        fh.seek(1, 1)
                else:
                    fh.seek(size + (size & 1), 1)
    except (OSError, struct.error):
        pass
    return out


def read_tags(path: str) -> dict[str, Any]:
    """{"artist", "album_artist", "album", "title", "genre", "bpm", "key",
    "track", "duration_s", "art": bytes|None} from the file's tags."""
    tags: dict[str, Any] = {"artist": "", "album_artist": "", "album": "", "title": "", "genre": "",
                            "bpm": 0.0, "key": "", "track": "", "duration_s": 0.0, "art": None}
    try:
        import mutagen
        f = mutagen.File(path)
    except Exception:  # noqa: BLE001 - an unreadable header is just "no tags"
        f = None
    if f is not None:
        info = getattr(f, "info", None)
        tags["duration_s"] = float(getattr(info, "length", 0.0) or 0.0)
        t = f.tags
        if t is not None:
            def get(*keys: str) -> str:
                for k in keys:
                    try:
                        if k in t:
                            v = _first(t[k])
                            if v:
                                return v
                    except (KeyError, ValueError, TypeError):
                        continue
                return ""
            tags["artist"] = get("TPE1", "artist", "ARTIST", "\xa9ART", "Author")
            tags["album_artist"] = get("TPE2", "albumartist", "ALBUMARTIST", "aART")
            tags["album"] = get("TALB", "album", "ALBUM", "\xa9alb")
            tags["title"] = get("TIT2", "title", "TITLE", "\xa9nam", "Title")
            tags["genre"] = get("TCON", "genre", "GENRE", "\xa9gen")
            tags["key"] = get("TKEY", "initialkey", "INITIALKEY", "key", "KEY")
            tags["track"] = get("TRCK", "tracknumber", "TRACKNUMBER", "trkn")
            bpm = get("TBPM", "bpm", "BPM", "tmpo")
            try:
                tags["bpm"] = float(re.findall(r"[\d.]+", bpm)[0]) if bpm else 0.0
            except (IndexError, ValueError):
                tags["bpm"] = 0.0
            tags["art"] = _embedded_art(f, t)
    if path.lower().endswith((".wav", ".wave")):
        info = _riff_info(path)
        tags["artist"] = tags["artist"] or info.get("IART", "")
        tags["title"] = tags["title"] or info.get("INAM", "")
        tags["genre"] = tags["genre"] or info.get("IGNR", "")
        tags["album"] = tags["album"] or info.get("IPRD", "")
    # ID3 genres like "(13)" or "Hip-Hop\x00Rap".
    g = re.sub(r"^\(\d+\)", "", str(tags["genre"])).replace("\x00", "/").strip()
    tags["genre"] = g
    return tags


def _embedded_art(f: Any, t: Any) -> bytes | None:
    try:
        if hasattr(t, "getall"):                       # ID3 (mp3 / wav / aiff)
            pics = t.getall("APIC")
            if pics:
                front = [p for p in pics if getattr(p, "type", 0) == 3]
                return bytes((front or pics)[0].data)
        pics = getattr(f, "pictures", None)            # FLAC
        if pics:
            return bytes(pics[0].data)
        if "covr" in t:                                # MP4
            covr = t["covr"]
            if covr:
                return bytes(covr[0])
        for key in ("metadata_block_picture", "METADATA_BLOCK_PICTURE"):   # Ogg / Opus
            if key in t:
                from mutagen.flac import Picture
                return bytes(Picture(base64.b64decode(_first(t[key]))).data)
    except Exception:  # noqa: BLE001 - broken artwork is no artwork
        return None
    return None


# --- folders: packs, authors, covers -----------------------------------------

class _DirInfo:
    """Cached facts about one folder (its listing is read once per scan)."""

    def __init__(self, path: str):
        self.path = path
        self.name = os.path.basename(path.rstrip("\\/")) or path
        self.images: list[str] = []
        self.generic_children = 0
        try:
            with os.scandir(path) as it:
                for e in it:
                    if e.is_file() and e.name.lower().endswith(IMAGE_EXTS):
                        self.images.append(e.path)
                    elif e.is_dir() and is_generic(e.name):
                        self.generic_children += 1
        except OSError:
            pass
        self.images.sort()

    def cover(self) -> str:
        by_stem = {Path(p).stem.lower(): p for p in self.images}
        for stem in COVER_STEMS:
            for s, p in by_stem.items():
                if s == stem or s.startswith(stem):
                    return p
        return self.images[0] if len(self.images) == 1 else ""


def is_generic(name: str) -> bool:
    n = norm(name)
    if n in GENERIC_DIRS:
        return True
    # "Kit 01", "Construction Kit 3 - 140bpm Am": a kit inside a pack.
    return bool(re.match(r"^(construction )?kit \d+\b", n) or re.match(r"^(ck|vol|volume|part) ?\d+$", n))


def _pack_evidence(d: _DirInfo) -> int:
    score = 0
    if d.cover():
        score += 2
    if d.generic_children >= 2:
        score += 2
    elif d.generic_children == 1:
        score += 1
    n = norm(d.name)
    if re.search(r"\b(pack|kit|vol|volume|collection|bundle|edition|construction|sample|samples|loops)\b", n):
        score += 1
    return score


class _Folders:
    def __init__(self) -> None:
        self._dirs: dict[str, _DirInfo] = {}

    def info(self, path: str) -> _DirInfo:
        d = self._dirs.get(path)
        if d is None:
            if len(self._dirs) > 20000:
                self._dirs.clear()
            d = self._dirs[path] = _DirInfo(path)
        return d

    def ancestors(self, file_path: str, root: str) -> list[str]:
        """Folders from the file's own up to ``root`` (inclusive)."""
        out = []
        d = os.path.dirname(file_path)
        root_n = os.path.normcase(os.path.abspath(root))
        for _ in range(64):
            out.append(d)
            if os.path.normcase(os.path.abspath(d)) == root_n:
                break
            parent = os.path.dirname(d)
            if parent == d:
                break
            d = parent
        return out

    def pack_of(self, file_path: str, root: str) -> str:
        """The construction / sample pack folder of a sample ('' when none)."""
        best, best_score = "", 0
        nearest = ""
        root_n = os.path.normcase(os.path.abspath(root))
        for d in self.ancestors(file_path, root):
            info = self.info(d)
            if is_generic(info.name):
                continue
            nearest = nearest or d
            if os.path.normcase(os.path.abspath(d)) == root_n:
                # The scan root is the user's library ("Samples", "Music"),
                # not a pack - unless it has a cover or is named like one
                # (the user pointed the scan at a single pack).
                if not (info.cover() or re.search(r"\b(pack|kit|vol|volume|collection|bundle)\b",
                                                  norm(info.name))):
                    continue
            s = _pack_evidence(info)
            if s >= best_score and s > 0:     # ties go to the higher folder
                best, best_score = d, s
        if best:
            return best
        if nearest and os.path.normcase(os.path.abspath(nearest)) != root_n:
            return nearest
        return ""

    def vendor_of(self, pack_path: str, root: str) -> str:
        """The label / vendor folder holding a pack, or the vendor in "Vendor - Pack"."""
        if not pack_path:
            return ""
        parent = os.path.dirname(pack_path)
        root_n = os.path.normcase(os.path.abspath(root))
        under_root = os.path.normcase(os.path.abspath(pack_path)) != root_n
        if under_root and os.path.normcase(os.path.abspath(parent)) != root_n and parent != pack_path:
            # Only folders strictly inside the root count (the root is the
            # user's own "Samples" or "Music" folder, not a vendor).
            pn = os.path.normcase(os.path.abspath(parent))
            if pn.startswith(root_n) and not is_generic(os.path.basename(parent)):
                return os.path.basename(parent)
        name = os.path.basename(pack_path)
        if " - " in name:
            return name.split(" - ", 1)[0].strip()
        return ""


# --- thumbnails ---------------------------------------------------------------

def make_thumbnail(data: bytes | None = None, image_path: str = "") -> str:
    """A THUMB_PX JPEG of the image (bytes or file) in the thumbnail cache;
    returns its path, or '' when there is no image or it can't be read."""
    try:
        if data is not None:
            key = hashlib.sha1(data).hexdigest()[:20]
        elif image_path:
            st = os.stat(image_path)
            key = hashlib.sha1(f"{os.path.normcase(image_path)}|{st.st_size}|{st.st_mtime_ns}"
                               .encode("utf-8")).hexdigest()[:20]
        else:
            return ""
        out = thumb_dir() / f"{key}.jpg"
        if out.is_file():
            return str(out)
        from PIL import Image  # Pillow comes with reportlab
        src = io.BytesIO(data) if data is not None else image_path
        with Image.open(src) as im:
            im = im.convert("RGB")
            im.thumbnail((THUMB_PX, THUMB_PX))
            out.parent.mkdir(parents=True, exist_ok=True)
            tmp = out.with_suffix(f".{os.getpid()}.tmp")
            im.save(tmp, "JPEG", quality=85)
            os.replace(tmp, out)
        return str(out)
    except Exception as exc:  # noqa: BLE001 - a broken image is no thumbnail
        _log.debug("No thumbnail (%s): %s", image_path or "embedded", exc)
        return ""


def thumbnail_payload(path: str) -> dict[str, Any] | None:
    """{"width", "height", "jpeg_base64"} of a cached thumbnail."""
    try:
        data = Path(path).read_bytes()
    except OSError:
        return None
    width = height = 0
    try:
        from PIL import Image
        with Image.open(io.BytesIO(data)) as im:
            width, height = im.size
    except Exception:  # noqa: BLE001
        pass
    return {"width": width, "height": height, "jpeg_base64": base64.b64encode(data).decode("ascii")}


# --- classification -----------------------------------------------------------

def _name_tokens(stem: str) -> str:
    return " " + norm(stem.replace("_", " ")) + " "


def parse_bpm(text: str) -> float:
    m = _BPM.search(text.replace("_", " "))
    if m:
        v = float(m.group(1))
        if 50.0 <= v <= 250.0:
            return v
    return 0.0


_BARE_TEMPO = re.compile(r"(?:^|[\s_\-])(\d{2,3})(?=$|[\s_\-])")


def parse_bare_bpm(text: str) -> float:
    """A loop's tempo written without "bpm" ("DHE_124_Bassline_Gm_loop")."""
    for m in _BARE_TEMPO.finditer(text):
        v = float(m.group(1))
        if 60.0 <= v <= 200.0:
            return v
    return 0.0


def parse_key(text: str) -> str:
    s = text.replace("_", " ")
    m = _KEY.search(s)
    if m:
        note = m.group(1).replace("sharp", "#").replace("flat", "b")
        mode = m.group(2).lower()
        return note + ("m" if mode in ("m", "min", "minor") else "")
    m = _KEY_BARE.search(s)
    return m.group(1) if m else ""


def classify(path: str, root: str, duration_s: float, tags: dict[str, Any]) -> tuple[str, str, float]:
    """(kind 'song' | 'sample', sample_type 'loop' | 'one_shot' | '', score).
    ``score`` >= 1.5 is a sample."""
    rel_dirs = [norm(p) for p in Path(os.path.relpath(os.path.dirname(path), os.path.dirname(root)
                                                      if os.path.dirname(root) else root)).parts]
    folder_text = " " + " ".join(rel_dirs) + " "
    stem = Path(path).stem
    name = _name_tokens(stem)
    score = 0.0
    if duration_s > 0:
        if duration_s < 20:
            score += 3.0
        elif duration_s < 45:
            score += 1.5
        elif duration_s > 120:
            score -= 3.0
        elif duration_s > 75:
            score -= 1.5
    if any(f" {w} " in folder_text for w in SAMPLE_FOLDER_WORDS):
        score += 2.0
    if any(f" {w} " in folder_text for w in MUSIC_FOLDER_WORDS):
        score -= 1.0
    loop_word = bool(re.search(r" (loop|loops|lp) ", name))
    shot_word = bool(re.search(r" (one ?shot|oneshot|os|shot) ", name))
    hit_word = any(f" {w} " in name for w in HIT_WORDS)
    bpm = parse_bpm(stem)
    if loop_word or shot_word or bpm or parse_key(stem):
        score += 1.5
    elif hit_word:
        score += 1.0
    if tags.get("artist") and tags.get("album"):
        score -= 1.5
    if tags.get("track"):
        score -= 1.0
    if score < 1.5:
        return "song", "", score
    if shot_word or (0 < duration_s < 2.0) or (hit_word and 0 < duration_s < 4.0 and not loop_word):
        return "sample", "one_shot", score
    if loop_word or bpm:
        return "sample", "loop", score
    return "sample", ("one_shot" if 0 < duration_s < 4.0 else "loop"), score


def clean_title(stem: str) -> str:
    t = _TRACKNO.sub("", stem).strip()
    return t or stem


# --- audio genre ----------------------------------------------------------------

def _excerpt(path: str, seconds: float = EXCERPT_S) -> tuple[np.ndarray, int] | None:
    """A mono excerpt from the middle of the file, read without decoding the
    whole file. Deliberately not ``load_audio``: a library scan touches
    thousands of files once each, which would only churn the process-wide
    decode cache (PERF-2) with whole-track arrays nothing reads again."""
    try:
        import soundfile as sf
        with sf.SoundFile(path) as fh:
            sr = int(fh.samplerate)
            n = int(fh.frames)
            want = int(seconds * sr)
            if n > want:
                fh.seek((n - want) // 2)
            y = fh.read(min(n, want), dtype="float32", always_2d=True)
        y = y.mean(axis=1)
    except Exception:  # noqa: BLE001 - formats libsndfile can't read (m4a)
        try:
            import librosa
            import soundfile as sf  # noqa: F401
            dur = 0.0
            try:
                import mutagen
                f = mutagen.File(path)
                dur = float(getattr(getattr(f, "info", None), "length", 0.0) or 0.0)
            except Exception:  # noqa: BLE001
                pass
            offset = max(0.0, (dur - seconds) / 2.0)
            y, sr = librosa.load(path, sr=22050, mono=True, offset=offset, duration=seconds)
            sr = int(sr)
        except Exception:  # noqa: BLE001
            return None
    if y.size < sr // 2 or not np.isfinite(y).all() or float(np.max(np.abs(y))) < 1e-5:
        return None
    return np.ascontiguousarray(y, dtype=np.float32), sr


def audio_genre(path: str, *, use_probe: bool, known_bpm: float = 0.0) -> dict[str, Any] | None:
    """{"genre", "family", "source", "confidence", "bpm"} from the audio: the
    genre probe when it can run (``use_probe``), else the ranker on an excerpt."""
    from khensu_services.analysis.learned import genre_probe
    if use_probe and genre_probe.available():
        try:
            pred = genre_probe.predict_file(path)
        except Exception:  # noqa: BLE001 - fall through to the ranker
            pred = None
        if pred and pred.get("genres"):
            top = pred["genres"][0]
            g, fam = resolve_genre(str(top["genre"]))
            if g or fam:
                return {"genre": g, "family": fam, "source": "probe",
                        "confidence": round(float(top.get("rank_score", 0.0)), 3), "bpm": known_bpm}
    ex = _excerpt(path)
    if ex is None:
        return None
    y, sr = ex
    bpm = known_bpm
    if bpm <= 0:
        try:
            from khensu_services.analysis import _lean_spectral as _lean
            tempo, _ = _lean.beat_track(y=y, sr=sr, units="frames")
            bpm = float(np.atleast_1d(tempo)[0])
        except Exception:  # noqa: BLE001
            bpm = 120.0
    from khensu_services.analysis.genre_detector import estimate_genre_blend
    blend = estimate_genre_blend(y, sr, bpm)
    if not blend:
        return None
    top = blend[0]
    g, fam = resolve_genre(str(top["genre"]))
    if not (g or fam):
        return None
    # The ranker's score is a rank, not a probability: its confidence is
    # capped at 0.5 so the UI shows it as a guess.
    second = float(blend[1]["rank_score"]) if len(blend) > 1 else 0.0
    first = float(top["rank_score"])
    margin = (first - second) / first if first > 0 else 0.0
    conf = round(min(0.5, 0.2 + 0.3 * first + 0.5 * margin), 3)
    return {"genre": g, "family": fam, "source": "ranker", "confidence": conf, "bpm": round(bpm, 1)}


# --- one file -----------------------------------------------------------------

def _duration(path: str, tags: dict[str, Any]) -> float:
    try:
        import soundfile as sf
        return float(sf.info(path).duration)
    except Exception:  # noqa: BLE001
        return float(tags.get("duration_s") or 0.0)


def analyse_file(path: str, root: str, folders: _Folders, *, use_probe: bool = True) -> dict[str, Any]:
    """Everything the scan stores about one file (no database access)."""
    st = os.stat(path)
    tags = read_tags(path)
    duration = _duration(path, tags)
    kind, sample_type, _score = classify(path, root, duration, tags)
    stem = Path(path).stem
    if not tags["bpm"] and kind == "sample" and sample_type == "loop" and not parse_bpm(stem):
        tags["bpm"] = parse_bare_bpm(stem)
    item: dict[str, Any] = {
        "id": item_id(path), "root": root, "path": os.path.abspath(path), "kind": kind,
        "sample_type": sample_type,
        "title": tags["title"] or (clean_title(stem) if kind == "song" else stem), "author": "",
        "album": tags["album"], "pack": "", "pack_path": "", "artwork_path": "", "genre": "", "family": "",
        "genre_source": "", "genre_confidence": 0.0, "tag_genre": tags["genre"],
        "bpm": float(tags["bpm"] or parse_bpm(stem)), "musical_key": tags["key"] or parse_key(stem),
        "duration_s": round(duration, 3), "size": int(st.st_size), "mtime_ns": int(st.st_mtime_ns),
        "indexed": 0, "error": "",
    }
    file_dir = folders.info(os.path.dirname(path))
    root_n = os.path.normcase(os.path.abspath(root))
    if kind == "sample":
        pack_path = folders.pack_of(path, root)
        item["pack_path"] = pack_path
        item["pack"] = os.path.basename(pack_path) if pack_path else ""
        item["author"] = tags["artist"] or folders.vendor_of(pack_path, root)
        if not item["album"]:
            item["album"] = item["pack"]
    else:
        item["author"] = tags["artist"] or tags["album_artist"]
        # Music/Artist/Album/01 Track.mp3
        parent = os.path.dirname(path)
        grand = os.path.dirname(parent)
        inside = [os.path.normcase(os.path.abspath(p)).startswith(root_n)
                  and os.path.normcase(os.path.abspath(p)) != root_n for p in (parent, grand)]
        if not item["album"] and inside[0] and not is_generic(os.path.basename(parent)):
            item["album"] = os.path.basename(parent)
        if not item["author"] and all(inside) and not is_generic(os.path.basename(grand)):
            item["author"] = os.path.basename(grand)
    # Artwork: embedded, else the file's folder, else the pack's folder.
    thumb = make_thumbnail(data=tags["art"]) if tags["art"] else ""
    if not thumb:
        cover = file_dir.cover()
        if not cover and item["pack_path"]:
            cover = folders.info(item["pack_path"]).cover()
        if cover:
            thumb = make_thumbnail(image_path=cover)
    item["artwork_path"] = thumb

    # Genre: tag, then folder names, then the audio.
    if tags["genre"]:
        g, fam, exact = match_genre_text(tags["genre"])
        if g or fam:
            item.update(genre=g, family=fam, genre_source="tag",
                        genre_confidence=(0.95 if exact else 0.85) if g else 0.6)
    if not item["genre"]:
        try:
            rel = Path(os.path.relpath(os.path.dirname(path), root)).parts
        except ValueError:
            rel = ()
        segments = [s for s in rel if s not in (".", "..")]
        if kind == "sample":
            segments.append(stem)
        for seg in reversed(segments):       # the nearest folder first
            g, fam, _exact = match_genre_text(seg, stop=FOLDER_GENRE_STOP)
            if g:
                if not item["family"] or item["family"] == fam:
                    item.update(genre=g, family=fam, genre_source="folder", genre_confidence=0.6)
                break
            if fam and not item["family"]:
                item.update(family=fam, genre_source="folder", genre_confidence=0.45)
    needs_audio = not item["genre"] and (kind == "song" or (sample_type == "loop" and duration >= 4.0))
    if needs_audio:
        ag = audio_genre(path, use_probe=(kind == "song" and use_probe), known_bpm=item["bpm"])
        if ag:
            if item["family"] and ag["family"] != item["family"]:
                pass   # the tag / folder family stands; the audio guess disagrees with it
            else:
                item.update(genre=ag["genre"], family=ag["family"], genre_source=ag["source"],
                            genre_confidence=float(ag["confidence"]))
            if not item["bpm"] and kind == "sample":
                item["bpm"] = float(ag.get("bpm") or 0.0)
    return item


# --- the database -----------------------------------------------------------------

_ddl_applied: set[str] = set()

_ITEM_COLUMNS = ("id", "root", "path", "kind", "sample_type", "title", "author", "album", "pack", "pack_path",
                 "artwork_path", "genre", "family", "genre_source", "genre_confidence", "tag_genre", "bpm",
                 "musical_key", "duration_s", "size", "mtime_ns", "indexed", "error", "scanned_at")


class _Conn:
    """``collab.db.connection()`` with migration 017 applied once per file."""

    def __enter__(self):
        from khensu_services.collab import db
        self._cm = db.connection()
        conn = self._cm.__enter__()
        path = db._db_path()
        if path not in _ddl_applied:
            from khensu_services.common.schema_sql import MIGRATIONS
            conn.executescript(MIGRATIONS[17])
            conn.commit()
            _ddl_applied.add(path)
        return conn

    def __exit__(self, *exc):
        return self._cm.__exit__(*exc)


def canonical_root(path: str) -> str:
    return os.path.normpath(os.path.abspath(path))


def list_roots() -> list[dict[str, Any]]:
    with _Conn() as conn:
        rows = conn.execute("SELECT path, added_at, last_scan_at, rescan_on_launch FROM reference_scan_roots "
                            "ORDER BY added_at").fetchall()
        counts = {r[0]: int(r[1]) for r in conn.execute(
            "SELECT root, COUNT(*) FROM reference_scan_items GROUP BY root").fetchall()}
    return [{"path": r[0], "added_at": r[1], "last_scan_at": r[2] or "", "rescan_on_launch": bool(r[3]),
             "exists": os.path.isdir(r[0]), "items": counts.get(r[0], 0)} for r in rows]


def set_root(path: str, rescan_on_launch: bool | None = None, *, replace: bool = False) -> dict[str, Any]:
    """Add (or update) a scan folder. ``replace`` makes it the only one (the
    screen's single "Reference folder"); the old folders' items are forgotten."""
    root = canonical_root(path)
    with _Conn() as conn:
        if replace:
            others = [r[0] for r in conn.execute("SELECT path FROM reference_scan_roots WHERE path != ?",
                                                 (root,)).fetchall()]
            for o in others:
                conn.execute("DELETE FROM reference_scan_items WHERE root = ?", (o,))
                conn.execute("DELETE FROM reference_scan_roots WHERE path = ?", (o,))
        conn.execute("INSERT INTO reference_scan_roots (path, added_at, rescan_on_launch) VALUES (?, ?, ?) "
                     "ON CONFLICT (path) DO NOTHING", (root, utc_now(), int(bool(rescan_on_launch))))
        if rescan_on_launch is not None:
            conn.execute("UPDATE reference_scan_roots SET rescan_on_launch = ? WHERE path = ?",
                         (int(bool(rescan_on_launch)), root))
        conn.commit()
    return next(r for r in list_roots() if r["path"] == root)


def remove_root(path: str, forget_items: bool = True) -> int:
    """Remove a scan folder; returns how many items were forgotten."""
    root = canonical_root(path)
    with _Conn() as conn:
        n = 0
        if forget_items:
            n = int(conn.execute("SELECT COUNT(*) FROM reference_scan_items WHERE root = ?", (root,)).fetchone()[0])
            conn.execute("DELETE FROM reference_scan_items WHERE root = ?", (root,))
        conn.execute("DELETE FROM reference_scan_roots WHERE path = ?", (root,))
        conn.commit()
    return n


def _known(root: str) -> dict[str, tuple[int, int, str, str]]:
    with _Conn() as conn:
        rows = conn.execute("SELECT path, size, mtime_ns, error, kind FROM reference_scan_items WHERE root = ?",
                            (root,)).fetchall()
    return {os.path.normcase(r[0]): (int(r[1]), int(r[2]), str(r[3]), str(r[4])) for r in rows}


def _upsert(conn, item: dict[str, Any]) -> None:
    row = {**item, "scanned_at": utc_now()}
    cols = ", ".join(_ITEM_COLUMNS)
    marks = ", ".join("?" for _ in _ITEM_COLUMNS)
    updates = ", ".join(f"{c} = excluded.{c}" for c in _ITEM_COLUMNS if c not in ("id",))
    # A genre the user corrected survives a rescan of the same file.
    conn.execute(f"INSERT INTO reference_scan_items ({cols}) VALUES ({marks}) ON CONFLICT (id) DO UPDATE SET "
                 f"{updates} WHERE reference_scan_items.genre_source != 'user'",
                 tuple(row[c] for c in _ITEM_COLUMNS))
    conn.execute("UPDATE reference_scan_items SET size = ?, mtime_ns = ?, scanned_at = ? "
                 "WHERE id = ? AND genre_source = 'user'",
                 (row["size"], row["mtime_ns"], row["scanned_at"], row["id"]))


def public_item(r: Any) -> dict[str, Any]:
    d = dict(r)
    return {
        "id": d["id"], "root": d["root"], "path": d["path"], "kind": d["kind"],
        "sample_type": d["sample_type"], "title": d["title"], "author": d["author"], "album": d["album"],
        "pack": d["pack"], "pack_path": d["pack_path"], "artwork_path": d["artwork_path"],
        "has_artwork": bool(d["artwork_path"]), "genre": d["genre"], "family": d["family"],
        "genre_source": d["genre_source"], "genre_confidence": float(d["genre_confidence"]),
        "tag_genre": d["tag_genre"], "bpm": float(d["bpm"]), "key": d["musical_key"],
        "duration_s": float(d["duration_s"]), "indexed": bool(d["indexed"]),
        "exists": os.path.isfile(d["path"]), "error": d["error"],
    }


def _where(genre: str | None, family: str | None, kind: str | None, pack: str | None, author: str | None,
           text: str | None, unsorted: bool) -> tuple[str, list[Any]]:
    clauses, args = [], []
    if genre:
        clauses.append("genre = ?")
        args.append(genre)
    if family:
        clauses.append("family = ?")
        args.append(family)
    if unsorted:
        clauses.append("genre = '' AND family = ''")
    if kind:
        clauses.append("kind = ?")
        args.append(kind)
    if pack:
        clauses.append("(pack = ? OR pack_path = ?)")
        args += [pack, pack]
    if author:
        clauses.append("author = ?")
        args.append(author)
    if text:
        like = f"%{text.strip().lower()}%"
        clauses.append("(LOWER(title) LIKE ? OR LOWER(author) LIKE ? OR LOWER(pack) LIKE ? "
                       "OR LOWER(album) LIKE ? OR LOWER(path) LIKE ?)")
        args += [like] * 5
    return (" WHERE " + " AND ".join(clauses)) if clauses else "", args


def list_items(*, genre: str | None = None, family: str | None = None, kind: str | None = None,
               pack: str | None = None, author: str | None = None, text: str | None = None,
               unsorted: bool = False, limit: int = 200, offset: int = 0) -> dict[str, Any]:
    where, args = _where(genre, family, kind, pack, author, text, unsorted)
    with _Conn() as conn:
        total = int(conn.execute(f"SELECT COUNT(*) FROM reference_scan_items{where}", args).fetchone()[0])
        rows = conn.execute(f"SELECT * FROM reference_scan_items{where} ORDER BY kind DESC, "
                            "genre_confidence DESC, LOWER(author), LOWER(pack), LOWER(title) LIMIT ? OFFSET ?",
                            [*args, int(limit), int(offset)]).fetchall()
    return {"items": [public_item(r) for r in rows], "total": total}


def list_packs(*, genre: str | None = None, family: str | None = None, author: str | None = None,
               text: str | None = None, unsorted: bool = False, limit: int = 200) -> list[dict[str, Any]]:
    where, args = _where(genre, family, "sample", None, author, text, unsorted)
    where += (" AND " if where else " WHERE ") + "pack_path != ''"
    with _Conn() as conn:
        rows = conn.execute(
            f"SELECT pack_path, MAX(pack), MAX(author), COUNT(*), MAX(artwork_path), "
            f"SUM(CASE WHEN sample_type = 'loop' THEN 1 ELSE 0 END) FROM reference_scan_items{where} "
            "GROUP BY pack_path ORDER BY LOWER(MAX(pack)) LIMIT ?", [*args, int(limit)]).fetchall()
        out = []
        for r in rows:
            g = conn.execute("SELECT genre, family, COUNT(*) FROM reference_scan_items WHERE pack_path = ? "
                             "AND (genre != '' OR family != '') GROUP BY genre, family ORDER BY 3 DESC LIMIT 1",
                             (r[0],)).fetchone()
            out.append({"pack": r[1] or os.path.basename(r[0]), "pack_path": r[0], "author": r[2] or "",
                        "samples": int(r[3]), "loops": int(r[5] or 0), "artwork_path": r[4] or "",
                        "has_artwork": bool(r[4]), "genre": g[0] if g else "", "family": g[1] if g else ""})
    return out


def summary() -> dict[str, Any]:
    with _Conn() as conn:
        kinds = dict(conn.execute("SELECT kind, COUNT(*) FROM reference_scan_items GROUP BY kind").fetchall())
        packs = int(conn.execute("SELECT COUNT(DISTINCT pack_path) FROM reference_scan_items "
                                 "WHERE kind = 'sample' AND pack_path != ''").fetchone()[0])
        unsorted = int(conn.execute("SELECT COUNT(*) FROM reference_scan_items WHERE genre = '' AND family = ''")
                       .fetchone()[0])
        genres = {str(g): int(n) for g, n in conn.execute(
            "SELECT genre, COUNT(*) FROM reference_scan_items WHERE genre != '' GROUP BY genre").fetchall()}
        families = {str(g): int(n) for g, n in conn.execute(
            "SELECT family, COUNT(*) FROM reference_scan_items WHERE family != '' GROUP BY family").fetchall()}
    return {"songs": int(kinds.get("song", 0)), "samples": int(kinds.get("sample", 0)), "packs": packs,
            "unsorted": unsorted, "genres": genres, "families": families}


def get_item(item_id_: str) -> dict[str, Any] | None:
    with _Conn() as conn:
        r = conn.execute("SELECT * FROM reference_scan_items WHERE id = ?", (item_id_,)).fetchone()
    return public_item(r) if r else None


def set_item_genre(item_id_: str, genre: str, *, apply_to_pack: bool = False, learn: bool = True) -> dict[str, Any]:
    """The user's genre for an item (a catalog genre or a family; '' clears
    it). Returns {"item", "updated", "learned"}. Raises KeyError for an
    unknown item, ValueError for an unknown genre."""
    item = get_item(item_id_)
    if item is None:
        raise KeyError(item_id_)
    if genre:
        g, fam = resolve_genre(genre)
        if not (g or fam):
            raise ValueError(f"unknown genre {genre!r}")
    else:
        g, fam = "", ""
    with _Conn() as conn:
        if apply_to_pack and item["kind"] == "sample" and item["pack_path"]:
            cur = conn.execute("UPDATE reference_scan_items SET genre = ?, family = ?, genre_source = ?, "
                               "genre_confidence = ? WHERE pack_path = ?",
                               (g, fam, "user" if genre else "", 1.0 if genre else 0.0, item["pack_path"]))
        else:
            cur = conn.execute("UPDATE reference_scan_items SET genre = ?, family = ?, genre_source = ?, "
                               "genre_confidence = ? WHERE id = ?",
                               (g, fam, "user" if genre else "", 1.0 if genre else 0.0, item_id_))
        updated = int(cur.rowcount)
        conn.commit()
    learned = False
    if item["kind"] == "song" and g and learn and item["exists"]:
        # GEN-1e: a genre the user chose for a track teaches the probe
        # (stored only when the probe can run; nothing else is kept).
        try:
            from khensu_services.analysis.learned import genre_examples
            learned = bool(genre_examples.add(item["path"], g, "chosen").get("stored"))
        except Exception as exc:  # noqa: BLE001 - the correction itself stands
            _log.info("Genre example not stored for %s: %s", item["path"], exc)
    if item["indexed"]:
        try:
            from khensu_services.analysis.embeddings import set_entry_meta
            set_entry_meta(item_id_, None, g or fam)
        except KeyError:
            pass
    return {"item": get_item(item_id_), "updated": updated, "learned": learned}


# --- the scan -----------------------------------------------------------------------

def _hidden(entry: os.DirEntry) -> bool:
    if entry.name.startswith((".", "$", "~")):
        return True
    if entry.name.lower() in SKIP_DIRS:
        return True
    try:
        attrs = getattr(entry.stat(follow_symlinks=False), "st_file_attributes", 0)
    except OSError:
        return True
    return bool(attrs & (getattr(_stat, "FILE_ATTRIBUTE_HIDDEN", 2) | getattr(_stat, "FILE_ATTRIBUTE_SYSTEM", 4)))


def walk_audio(root: str, *, max_files: int = DEFAULT_MAX_FILES,
               cancelled: Callable[[], bool] = lambda: False) -> tuple[list[tuple[str, int, int]], bool]:
    """Every audio file under ``root`` as (path, size, mtime_ns), skipping
    hidden / system folders and links; ``complete`` is False when cancelled
    or cut at ``max_files``."""
    out: list[tuple[str, int, int]] = []
    stack = [root]
    while stack:
        if cancelled():
            return out, False
        d = stack.pop()
        try:
            with os.scandir(d) as it:
                entries = sorted(it, key=lambda e: e.name.lower())
        except OSError:
            continue
        subdirs = []
        for e in entries:
            try:
                if e.is_symlink() or (hasattr(os.path, "isjunction") and os.path.isjunction(e.path)):
                    continue
                if e.is_dir(follow_symlinks=False):
                    if not _hidden(e):
                        subdirs.append(e.path)
                elif e.is_file(follow_symlinks=False) and os.path.splitext(e.name)[1].lower() in AUDIO_EXTS:
                    if e.name.startswith(("._", ".")):
                        continue
                    st = e.stat(follow_symlinks=False)
                    out.append((e.path, int(st.st_size), int(st.st_mtime_ns)))
                    if len(out) >= max_files:
                        return out, False
            except OSError:
                continue
        stack.extend(reversed(subdirs))
    return out, True


def _propagate_pack_genres(conn, roots: Iterable[str]) -> int:
    """Samples without a genre take the most common genre of their pack."""
    n = 0
    for root in roots:
        packs = conn.execute("SELECT DISTINCT pack_path FROM reference_scan_items WHERE root = ? AND "
                             "kind = 'sample' AND pack_path != '' AND genre = '' AND family = ''",
                             (root,)).fetchall()
        for (pack_path,) in packs:
            rows = conn.execute("SELECT genre, family, genre_confidence FROM reference_scan_items "
                                "WHERE pack_path = ? AND (genre != '' OR family != '') AND genre_source != 'pack'",
                                (pack_path,)).fetchall()
            if not rows:
                continue
            votes: Counter = Counter()
            for g, fam, c in rows:
                votes[(g, fam)] += max(0.1, float(c))
            (g, fam), _w = votes.most_common(1)[0]
            share = _w / sum(votes.values())
            cur = conn.execute("UPDATE reference_scan_items SET genre = ?, family = ?, genre_source = 'pack', "
                               "genre_confidence = ? WHERE pack_path = ? AND genre = '' AND family = ''",
                               (g, fam, round(0.5 * share, 3), pack_path))
            n += int(cur.rowcount)
    return n


def _index_songs(conn_factory, songs: list[dict[str, Any]], cancelled: Callable[[], bool],
                 on_indexed: Callable[[int], None]) -> int:
    """Add songs to the reference library (embeddings index) with their genre."""
    from khensu_services.analysis import embeddings as emb
    try:
        backend = emb.backend_status("auto")["backend"]
    except emb.EmbeddingBackendUnavailable:
        backend = emb.BACKEND_HANDCRAFTED
    done = 0
    for i in range(0, len(songs), 8):
        if cancelled():
            break
        batch = songs[i:i + 8]
        try:
            res = emb.index_files([s["path"] for s in batch], [s["title"] for s in batch], backend)
        except emb.EmbeddingBackendUnavailable:
            backend = emb.BACKEND_HANDCRAFTED
            res = emb.index_files([s["path"] for s in batch], [s["title"] for s in batch], backend)
        ok = {r["id"] for r in res["results"] if r["status"] != "failed"}
        entries = {e["id"]: e for e in emb.list_index(backend)}
        with conn_factory() as conn:
            for s in batch:
                if s["id"] not in ok:
                    continue
                g = s["genre"] or s["family"]
                e = entries.get(s["id"])
                tags = list(e.get("tags", [])) if e else []
                if "scanned" not in tags:
                    tags.append("scanned")
                try:
                    emb.set_entry_meta(s["id"], tags, g if (g and not (e and e.get("genre"))) else None)
                except KeyError:
                    pass
                conn.execute("UPDATE reference_scan_items SET indexed = 1 WHERE id = ?", (s["id"],))
                done += 1
            conn.commit()
        on_indexed(done)
    return done


def scan(roots: list[str], *, index_songs: bool = True, max_files: int = DEFAULT_MAX_FILES,
         use_probe: bool = True,
         progress: Callable[[dict[str, Any]], None] = lambda p: None,
         cancelled: Callable[[], bool] = lambda: False) -> dict[str, Any]:
    """Scan ``roots`` (saved scan folders). Returns the counts; partial
    counts when ``cancelled`` turns True (``cancelled`` key)."""
    stats = {"files_found": 0, "processed": 0, "unchanged": 0, "songs": 0, "samples": 0, "failed": 0,
             "removed": 0, "indexed": 0, "cancelled": False, "truncated": False}
    state: dict[str, Any] = {"phase": "walking", "done": 0, "total": 0, "current": ""}

    def report() -> None:
        progress({**state, **{k: stats[k] for k in ("songs", "samples", "failed", "unchanged", "indexed")}})

    work: list[tuple[str, str, int, int]] = []
    complete_roots = []
    for root in roots:
        files, complete = walk_audio(root, max_files=max(1, max_files - len(work)), cancelled=cancelled)
        if cancelled():
            stats["cancelled"] = True
            return stats
        if complete:
            complete_roots.append(root)
        else:
            stats["truncated"] = True
        work += [(root, p, s, m) for p, s, m in files]
        state["total"] = len(work)
        report()
    stats["files_found"] = len(work)
    state.update(phase="reading", total=len(work))
    report()
    folders = _Folders()
    to_index: list[dict[str, Any]] = []
    seen: dict[str, set[str]] = {r: set() for r in roots}
    known = {r: _known(r) for r in roots}
    batch: list[dict[str, Any]] = []
    last_report = 0.0

    def flush() -> None:
        # Short write transactions: the shared khensu.db handle is not held
        # while files are read, so the screen can list items mid-scan.
        if batch:
            with _Conn() as conn:
                for it in batch:
                    _upsert(conn, it)
                conn.commit()
            batch.clear()

    for i, (root, path, size, mtime) in enumerate(work):
        if cancelled():
            stats["cancelled"] = True
            break
        key = os.path.normcase(path)
        seen[root].add(key)
        state.update(done=i, current=os.path.basename(path))
        old = known[root].get(key)
        if old is not None and old[0] == size and old[1] == mtime and not old[2]:
            stats["unchanged"] += 1
            stats["songs" if old[3] == "song" else "samples"] += 1
        else:
            try:
                item = analyse_file(path, root, folders, use_probe=use_probe)
            except Exception as exc:  # noqa: BLE001 - one bad file must not stop the scan
                _log.info("Reference scan: %s failed: %s", path, exc)
                stats["failed"] += 1
                item = {"id": item_id(path), "root": root, "path": os.path.abspath(path), "kind": "song",
                        "sample_type": "", "title": Path(path).stem, "author": "", "album": "", "pack": "",
                        "pack_path": "", "artwork_path": "", "genre": "", "family": "", "genre_source": "",
                        "genre_confidence": 0.0, "tag_genre": "", "bpm": 0.0, "musical_key": "",
                        "duration_s": 0.0, "size": size, "mtime_ns": mtime, "indexed": 0,
                        "error": f"{type(exc).__name__}: {exc}"[:200]}
            batch.append(item)
            stats["processed"] += 1
            if not item["error"]:
                stats["songs" if item["kind"] == "song" else "samples"] += 1
            if item["kind"] == "song" and not item["error"]:
                to_index.append(item)
            if len(batch) >= 25:
                flush()
        now = time.monotonic()
        if now - last_report > 0.25:
            last_report = now
            report()
    flush()
    with _Conn() as conn:
        # Rows for files that are gone (only for roots walked completely).
        if not stats["cancelled"]:
            for root in complete_roots:
                gone = [k for k in known[root] if k not in seen[root]]
                for k in gone:
                    conn.execute("DELETE FROM reference_scan_items WHERE root = ? AND path = ? COLLATE NOCASE",
                                 (root, k))
                stats["removed"] += len(gone)
            _propagate_pack_genres(conn, roots)
            for root in roots:
                conn.execute("UPDATE reference_scan_roots SET last_scan_at = ? WHERE path = ?", (utc_now(), root))
        conn.commit()
        stats["songs"] = stats["samples"] = 0
        for kind, n in conn.execute("SELECT kind, COUNT(*) FROM reference_scan_items GROUP BY kind").fetchall():
            stats["songs" if kind == "song" else "samples"] = int(n)
    if index_songs and to_index and not stats["cancelled"]:
        # Songs the user corrected keep the user's genre in the index too.
        fresh = {i["id"]: i for i in (get_item(s["id"]) for s in to_index) if i}
        songs = [fresh.get(s["id"], s) for s in to_index]
        state.update(phase="indexing", done=0, total=len(songs), current="")
        report()

        def on_indexed(n: int) -> None:
            stats["indexed"] = n
            state.update(done=n)
            report()
        stats["indexed"] = _index_songs(_Conn, songs, cancelled, on_indexed)
        if cancelled():
            stats["cancelled"] = True
    state.update(phase="done", done=state["total"], current="")
    report()
    return stats
