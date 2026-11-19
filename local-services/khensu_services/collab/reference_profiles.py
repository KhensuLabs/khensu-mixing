"""Shared reference profiles (CE-COL-4): numbers only, never audio.

A reference profile is the analysed metric vector of a reference track - the
same four numbers the reference library and benchmark use (integrated
loudness, loudness range, spectral tilt, crest factor) plus a name and a genre.
Profiles live in the user's library (``reference_profiles.json`` in the config
dir), are exported as hashed JSON, and are imported only after the hash checks
out and every value passes the allow-list (no paths, no audio, finite numbers
in plausible ranges).

An optional **index** - a JSON list of profile files at an https URL - can be
browsed and fetched. That is the only network use here and it goes through
``common/online.py`` (master switch + ``reference_index``).
"""
from __future__ import annotations

import hashlib
import json
import math
import os
import re
import threading
import uuid
from pathlib import Path
from typing import Any

from khensu_services.collab.db import utc_now

FILE_FORMAT = "khensu-reference-profiles"
INDEX_FORMAT = "khensu-reference-index"
FORMAT_VERSION = 1
MAX_PROFILES_PER_FILE = 500
MAX_LIBRARY_PROFILES = 5000
MAX_FILE_BYTES = 2 * 1024 * 1024
MAX_INDEX_ENTRIES = 1000
FETCH_TIMEOUT_S = 8.0

# metric -> plausible (min, max). Anything outside is refused, not clamped.
METRIC_RANGES: dict[str, tuple[float, float]] = {
    "lufs_integrated": (-70.0, 5.0),
    "lra": (0.0, 40.0),
    "spectral_tilt_db_per_octave": (-24.0, 24.0),
    "crest_factor_db": (0.0, 40.0),
}
_PROFILE_KEYS = {"name", "genre", "metrics", "analysed_at", "profile_sha256", "notes"}
_GENRE_RE = re.compile(r"^[a-z0-9][a-z0-9_ \-]{0,63}$")
_SHA_RE = re.compile(r"^[0-9a-f]{64}$")

_lock = threading.Lock()


class ProfileError(ValueError):
    """A profile file or value is invalid (mapped to 422)."""


def library_path() -> Path:
    override = os.environ.get("KHENSU_REFERENCE_PROFILES", "").strip()
    if override:
        return Path(override)
    from khensu_services.common.config import config_dir
    return config_dir() / "reference_profiles.json"


def _canonical(obj: Any) -> bytes:
    return json.dumps(obj, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode("ascii")


def profile_hash(profile: dict[str, Any]) -> str:
    """Hash of the content that matters (name, genre, metrics) - stable across
    key order and whitespace."""
    core = {"name": profile["name"], "genre": profile["genre"], "metrics": profile["metrics"]}
    return hashlib.sha256(_canonical(core)).hexdigest()


def validate_profile(raw: Any) -> dict[str, Any]:
    if not isinstance(raw, dict):
        raise ProfileError("A profile must be a JSON object.")
    unknown = set(raw) - _PROFILE_KEYS
    if unknown:
        raise ProfileError(f"Profile has fields that are not allowed: {sorted(unknown)} "
                           "(profiles carry numbers only - no paths, no audio).")
    name = raw.get("name")
    if not isinstance(name, str) or not name.strip() or len(name) > 80:
        raise ProfileError("Profile name must be 1-80 characters.")
    genre = raw.get("genre", "")
    if not isinstance(genre, str) or (genre and not _GENRE_RE.match(genre)):
        raise ProfileError("Profile genre must be a lowercase genre id.")
    metrics = raw.get("metrics")
    if not isinstance(metrics, dict) or set(metrics) - set(METRIC_RANGES):
        raise ProfileError(f"Profile metrics must be among {sorted(METRIC_RANGES)}.")
    clean_metrics: dict[str, float] = {}
    for key, (lo, hi) in METRIC_RANGES.items():
        if key not in metrics:
            raise ProfileError(f"Profile is missing metric '{key}'.")
        v = metrics[key]
        if isinstance(v, bool) or not isinstance(v, (int, float)) or not math.isfinite(float(v)):
            raise ProfileError(f"Metric '{key}' must be a finite number.")
        if not lo <= float(v) <= hi:
            raise ProfileError(f"Metric '{key}' = {v} is outside the plausible range {lo}..{hi}.")
        clean_metrics[key] = round(float(v), 4)
    notes = raw.get("notes", "")
    if not isinstance(notes, str) or len(notes) > 500:
        raise ProfileError("Profile notes must be text of at most 500 characters.")
    out = {"name": name.strip(), "genre": genre, "metrics": clean_metrics}
    if notes:
        out["notes"] = notes
    analysed_at = raw.get("analysed_at")
    if isinstance(analysed_at, str) and len(analysed_at) <= 40:
        out["analysed_at"] = analysed_at
    claimed = raw.get("profile_sha256")
    actual = profile_hash(out)
    if claimed is not None and claimed != actual:
        raise ProfileError(f"Profile '{out['name']}' does not match its hash (edited or damaged).")
    out["profile_sha256"] = actual
    return out


# --- the user's library ---------------------------------------------------

def _load_library() -> list[dict[str, Any]]:
    p = library_path()
    try:
        data = json.loads(p.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return []
    items = data.get("profiles") if isinstance(data, dict) else None
    return [x for x in items if isinstance(x, dict)] if isinstance(items, list) else []


def _save_library(items: list[dict[str, Any]]) -> None:
    p = library_path()
    p.parent.mkdir(parents=True, exist_ok=True)
    tmp = p.with_name(p.name + f".{uuid.uuid4().hex[:8]}.tmp")
    tmp.write_text(json.dumps({"format": "khensu-reference-library", "version": FORMAT_VERSION,
                               "profiles": items}, indent=1), encoding="utf-8")
    os.replace(tmp, p)


def list_profiles(genre: str | None = None) -> list[dict[str, Any]]:
    with _lock:
        items = _load_library()
    return [x for x in items if genre is None or x.get("genre") == genre]


def _add(profiles: list[dict[str, Any]], source: str) -> tuple[list[dict[str, Any]], int]:
    """Add validated profiles; a profile whose hash is already in the
    library is skipped (re-import is idempotent)."""
    with _lock:
        items = _load_library()
        known = {x.get("profile_sha256") for x in items}
        added = []
        for prof in profiles:
            if prof["profile_sha256"] in known:
                continue
            if len(items) >= MAX_LIBRARY_PROFILES:
                raise ProfileError(f"The library holds at most {MAX_LIBRARY_PROFILES} profiles.")
            entry = {**prof, "id": uuid.uuid4().hex, "source": source, "added_at": utc_now()}
            items.append(entry)
            known.add(prof["profile_sha256"])
            added.append(entry)
        _save_library(items)
    return added, len(profiles) - len(added)


def delete_profile(profile_id: str) -> bool:
    with _lock:
        items = _load_library()
        kept = [x for x in items if x.get("id") != profile_id]
        if len(kept) == len(items):
            return False
        _save_library(kept)
        return True


def analyse_profile(file_path: str, name: str, genre: str = "") -> dict[str, Any]:
    """Measure a reference track and add its numbers (not its audio) to the
    library. Blocking - run it off the event loop."""
    from khensu_services.analysis.reference_retrieval import target_profile
    measured = target_profile(file_path)
    metrics = {k: float(measured[k]) for k in METRIC_RANGES}
    prof = validate_profile({"name": name or Path(file_path).stem, "genre": genre,
                             "metrics": metrics, "analysed_at": utc_now()})
    added, _ = _add([prof], "analysed")
    if added:
        return added[0]
    return next(x for x in list_profiles() if x.get("profile_sha256") == prof["profile_sha256"])


# --- files ------------------------------------------------------------------

def build_export(profile_ids: list[str] | None = None) -> dict[str, Any]:
    items = list_profiles()
    if profile_ids:
        wanted = set(profile_ids)
        items = [x for x in items if x.get("id") in wanted]
    profiles = []
    for x in items:
        p = {"name": x["name"], "genre": x.get("genre", ""), "metrics": x["metrics"],
             "profile_sha256": x["profile_sha256"]}
        if x.get("analysed_at"):
            p["analysed_at"] = x["analysed_at"]
        if x.get("notes"):
            p["notes"] = x["notes"]
        profiles.append(p)
    return {"format": FILE_FORMAT, "version": FORMAT_VERSION, "exported_at": utc_now(),
            "profiles": profiles, "sha256": hashlib.sha256(_canonical(profiles)).hexdigest()}


def export_profiles(output_path: str, profile_ids: list[str] | None = None) -> dict[str, Any]:
    doc = build_export(profile_ids)
    if not doc["profiles"]:
        raise ProfileError("There are no profiles to export.")
    out = Path(output_path)
    out.parent.mkdir(parents=True, exist_ok=True)
    tmp = out.with_name(out.name + f".{uuid.uuid4().hex[:8]}.tmp")
    tmp.write_text(json.dumps(doc, indent=1), encoding="utf-8")
    os.replace(tmp, out)
    return {"output_path": str(out), "count": len(doc["profiles"]), "sha256": doc["sha256"]}


def parse_profile_file(data: bytes) -> list[dict[str, Any]]:
    if len(data) > MAX_FILE_BYTES:
        raise ProfileError("The profile file is larger than 2 MB.")
    try:
        doc = json.loads(data.decode("utf-8"))
    except (UnicodeDecodeError, ValueError) as exc:
        raise ProfileError("The profile file is not valid UTF-8 JSON.") from exc
    if not isinstance(doc, dict) or doc.get("format") != FILE_FORMAT:
        raise ProfileError("Not a Khensu reference-profile file.")
    if not isinstance(doc.get("version"), int) or doc["version"] > FORMAT_VERSION:
        raise ProfileError("Unsupported reference-profile file version.")
    raw = doc.get("profiles")
    if not isinstance(raw, list) or not raw or len(raw) > MAX_PROFILES_PER_FILE:
        raise ProfileError(f"A profile file holds 1-{MAX_PROFILES_PER_FILE} profiles.")
    claimed = doc.get("sha256")
    if not isinstance(claimed, str) or claimed != hashlib.sha256(_canonical(raw)).hexdigest():
        raise ProfileError("The profile file does not match its hash (edited or damaged).")
    return [validate_profile(p) for p in raw]


def import_profiles(path: str) -> dict[str, Any]:
    p = Path(path)
    if p.stat().st_size > MAX_FILE_BYTES:
        raise ProfileError("The profile file is larger than 2 MB.")
    profiles = parse_profile_file(p.read_bytes())
    added, skipped = _add(profiles, "imported")
    return {"imported": len(added), "skipped_duplicates": skipped, "profiles": added}


# --- optional index (online-gated by the caller) ---------------------------

def _https_or_loopback(url: str) -> bool:
    from khensu_services.common.online import is_loopback_http_url
    u = (url or "").strip()
    return u.lower().startswith("https://") or is_loopback_http_url(u)


def _fetch_bytes(url: str) -> bytes:
    from khensu_services.common.http_safety import safe_httpx_client
    with safe_httpx_client(timeout=FETCH_TIMEOUT_S) as client:
        with client.stream("GET", url, headers={"Accept": "application/json"}) as resp:
            if resp.status_code != 200:
                raise ProfileError(f"The index answered HTTP {resp.status_code}.")
            buf = bytearray()
            for chunk in resp.iter_bytes():
                buf.extend(chunk)
                if len(buf) > MAX_FILE_BYTES:
                    raise ProfileError("The index response is larger than 2 MB.")
            return bytes(buf)


def browse_index(index_url: str, fetch=_fetch_bytes) -> list[dict[str, Any]]:
    if not _https_or_loopback(index_url):
        raise ProfileError("The index URL must be https.")
    try:
        doc = json.loads(fetch(index_url).decode("utf-8"))
    except (UnicodeDecodeError, ValueError) as exc:
        raise ProfileError("The index is not valid JSON.") from exc
    if not isinstance(doc, dict) or doc.get("format") != INDEX_FORMAT:
        raise ProfileError("Not a Khensu reference-profile index.")
    entries = doc.get("entries")
    if not isinstance(entries, list) or len(entries) > MAX_INDEX_ENTRIES:
        raise ProfileError("The index has no valid entry list.")
    out = []
    for e in entries:
        if not isinstance(e, dict):
            continue
        url, sha, name = e.get("url"), e.get("sha256"), e.get("name")
        if not (isinstance(url, str) and _https_or_loopback(url) and isinstance(sha, str)
                and _SHA_RE.match(sha) and isinstance(name, str) and name.strip()):
            continue
        item = {"name": name.strip()[:80], "url": url, "sha256": sha}
        genre = e.get("genre")
        if isinstance(genre, str) and _GENRE_RE.match(genre):
            item["genre"] = genre
        count = e.get("count")
        if isinstance(count, int) and not isinstance(count, bool) and count >= 0:
            item["count"] = count
        out.append(item)
    return out


def fetch_from_index(url: str, sha256: str, fetch=_fetch_bytes) -> dict[str, Any]:
    if not _https_or_loopback(url):
        raise ProfileError("Profile URLs must be https.")
    if not _SHA_RE.match(sha256 or ""):
        raise ProfileError("A 64-character hex sha256 is required.")
    data = fetch(url)
    if hashlib.sha256(data).hexdigest() != sha256:
        raise ProfileError("The downloaded file does not match the index's sha256.")
    profiles = parse_profile_file(data)
    added, skipped = _add(profiles, "index")
    return {"imported": len(added), "skipped_duplicates": skipped, "profiles": added}
