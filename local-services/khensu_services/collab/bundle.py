"""`.khensu` session bundles (CE-COL-1, CE-COL-2 branch export).

A bundle is a zip archive that carries a session to another machine or
person:

    manifest.json           format, version, session name, contents + hashes
    analysis.json           the desktop's session export (session + analysis
                            results), absolute paths reduced to file names
    comments.json           timestamped comments
    render_versions.json    render-tree metadata (labels, tags, branches)
    audio/<name>            optional: the source and render audio

Import is fail-closed. Every member name must be on the allow-list above (no
directories, no ``..``, no absolute or drive paths, no backslashes, no
symlinks, no duplicates), sizes are capped per member and in total (headers
are not trusted: extraction counts the bytes it actually writes), the zip-bomb
ratio is bounded, and every member's SHA-256 must match the manifest. Audio is
written only under the caller's destination directory, through a temporary
file that is renamed into place once its hash checks out.
"""
from __future__ import annotations

import hashlib
import io
import json
import os
import re
import stat
import tempfile
import uuid
import zipfile
from pathlib import Path
from typing import Any

from khensu_services.collab import comments as comments_store
from khensu_services.collab import versions as versions_store
from khensu_services.collab.db import utc_now

FORMAT_NAME = "khensu-bundle"
FORMAT_VERSION = 1
BUNDLE_EXT = ".khensu"

MAX_BUNDLE_BYTES = 4 * 1024 ** 3          # the .khensu file on disk
MAX_MEMBERS = 256
MAX_JSON_BYTES = 32 * 1024 ** 2           # each JSON member, uncompressed
MAX_AUDIO_BYTES = 2 * 1024 ** 3           # each audio member, uncompressed
MAX_TOTAL_BYTES = 6 * 1024 ** 3           # everything, uncompressed
MAX_RATIO = 200                           # uncompressed / compressed, per member
MAX_COMMENTS = comments_store.MAX_COMMENTS_PER_SESSION
MAX_VERSIONS = versions_store.MAX_VERSIONS_PER_SESSION

_JSON_MEMBERS = ("manifest.json", "analysis.json", "comments.json", "render_versions.json")
_AUDIO_EXTS = (".wav", ".flac", ".aif", ".aiff", ".mp3", ".ogg", ".m4a")
_AUDIO_NAME_RE = re.compile(r"^audio/[A-Za-z0-9][A-Za-z0-9 _.()\-]{0,119}$")
_CHUNK = 1024 * 1024


class BundleError(ValueError):
    """The bundle is malformed or unsafe (mapped to 422)."""


class BundleTooLarge(BundleError):
    """A size limit was exceeded (mapped to 413)."""


# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------

def _sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _sha256_file(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(_CHUNK), b""):
            h.update(chunk)
    return h.hexdigest()


def _safe_audio_name(original: str, taken: set[str]) -> str:
    stem, ext = os.path.splitext(os.path.basename(original))
    ext = ext.lower()
    if ext not in _AUDIO_EXTS:
        raise BundleError(f"Not a bundleable audio type: {os.path.basename(original)}")
    clean = re.sub(r"[^A-Za-z0-9 _.()\-]", "_", stem).strip(" .") or "audio"
    clean = clean.lstrip("._- ") or "audio"
    clean = clean[: 110]
    name = f"audio/{clean}{ext}"
    n = 2
    while name.lower() in taken:
        name = f"audio/{clean}_{n}{ext}"
        n += 1
    taken.add(name.lower())
    return name


_PATH_KEYS = re.compile(r"(^|_)(path|file|dir|directory|folder)s?$", re.IGNORECASE)


def strip_local_paths(value: Any, key: str = "") -> Any:
    """Reduce every path-like string to its file name, so a shared bundle does
    not leak the sender's folder layout (user name, drive, project tree)."""
    if isinstance(value, dict):
        return {k: strip_local_paths(v, str(k)) for k, v in value.items()}
    if isinstance(value, list):
        return [strip_local_paths(v, key) for v in value]
    if isinstance(value, str) and key and _PATH_KEYS.search(key):
        return os.path.basename(value.replace("\\", "/"))
    return value


# ---------------------------------------------------------------------------
# export
# ---------------------------------------------------------------------------

def export_bundle(session_id: str, output_path: str, session_name: str = "",
                  session_export: dict[str, Any] | None = None,
                  source_paths: list[str] | None = None, include_audio: bool = False,
                  branch: str | None = None, service_version: str = "") -> dict[str, Any]:
    """Write a .khensu bundle for `session_id` to `output_path` (already
    validated by the caller). `branch` = None exports every version; a name
    ('' for main) exports that branch's lineage only."""
    if branch is None:
        versions = versions_store.list_versions(session_id)
        branches = versions_store.list_branches(session_id)
    else:
        versions = versions_store.branch_lineage(session_id, branch)
        branches = [b for b in versions_store.list_branches(session_id) if b["name"] == branch]
    comments = comments_store.list_comments(session_id)

    taken: set[str] = set()
    audio_entries: list[dict[str, Any]] = []
    path_to_member: dict[str, str] = {}
    total = 0

    def add_audio(path: str, role: str, version_id: str | None = None) -> None:
        nonlocal total
        if not path or path in path_to_member or not os.path.isfile(path):
            return
        size = os.path.getsize(path)
        if size > MAX_AUDIO_BYTES:
            raise BundleTooLarge(f"{os.path.basename(path)} is larger than the bundle's per-file limit.")
        total += size
        if total > MAX_TOTAL_BYTES:
            raise BundleTooLarge("The audio is larger than the bundle limit; export without audio.")
        member = _safe_audio_name(path, taken)
        path_to_member[path] = member
        entry: dict[str, Any] = {"name": member, "role": role, "bytes": size,
                                 "sha256": _sha256_file(path),
                                 "original_name": os.path.basename(path)}
        if version_id:
            entry["version_id"] = version_id
        audio_entries.append(entry)

    if include_audio:
        for p in source_paths or []:
            add_audio(p, "source")
        for v in versions:
            add_audio(v["path"], "render", v["id"])
        if len(audio_entries) + len(_JSON_MEMBERS) > MAX_MEMBERS:
            raise BundleTooLarge("Too many audio files for one bundle.")

    version_docs = []
    for v in versions:
        doc = {k: v[k] for k in ("id", "label", "tags", "branch", "created_at") if k in v}
        if v.get("parent_id"):
            doc["parent_id"] = v["parent_id"]
        doc["file_name"] = os.path.basename(v["path"])
        if v["path"] in path_to_member:
            doc["audio"] = path_to_member[v["path"]]
        version_docs.append(doc)
    comment_docs = [{k: c[k] for k in ("id", "author", "start_s", "end_s", "text", "resolved",
                                       "created_at") if k in c} for c in comments]

    json_members = {
        "analysis.json": json.dumps(strip_local_paths(session_export or {}), indent=1).encode("utf-8"),
        "comments.json": json.dumps({"comments": comment_docs}, indent=1).encode("utf-8"),
        "render_versions.json": json.dumps({"versions": version_docs, "branches": branches},
                                           indent=1).encode("utf-8"),
    }
    for name, data in json_members.items():
        if len(data) > MAX_JSON_BYTES:
            raise BundleTooLarge(f"{name} is larger than the bundle limit.")

    manifest = {
        "format": FORMAT_NAME,
        "format_version": FORMAT_VERSION,
        "created_at": utc_now(),
        "app": "Khensu Mixing",
        "service_version": service_version,
        "session": {"id": session_id, "name": session_name},
        "branch": branch,
        "files": {name: {"sha256": _sha256_bytes(data), "bytes": len(data)}
                  for name, data in json_members.items()},
        "audio": audio_entries,
        "counts": {"comments": len(comment_docs), "versions": len(version_docs),
                   "audio": len(audio_entries)},
    }

    out = Path(output_path)
    out.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp = tempfile.mkstemp(prefix=".khensu-", suffix=".part", dir=str(out.parent))
    os.close(fd)
    try:
        with zipfile.ZipFile(tmp, "w", compression=zipfile.ZIP_DEFLATED, allowZip64=True) as zf:
            zf.writestr("manifest.json", json.dumps(manifest, indent=1))
            for name, data in json_members.items():
                zf.writestr(name, data)
            for entry in audio_entries:
                src = next(p for p, m in path_to_member.items() if m == entry["name"])
                # Audio barely compresses; storing it keeps export fast.
                zf.write(src, entry["name"], compress_type=zipfile.ZIP_STORED)
        os.replace(tmp, out)
    except BaseException:
        try:
            os.remove(tmp)
        except OSError:
            pass
        raise
    return {"output_path": str(out), "bytes": out.stat().st_size,
            "comments": len(comment_docs), "versions": len(version_docs),
            "audio_files": len(audio_entries), "branch": branch}


# ---------------------------------------------------------------------------
# reading / validation
# ---------------------------------------------------------------------------

def _check_member_name(info: zipfile.ZipInfo) -> str:
    name = info.filename
    if name in _JSON_MEMBERS:
        kind = "json"
    elif _AUDIO_NAME_RE.match(name) and os.path.splitext(name)[1].lower() in _AUDIO_EXTS \
            and ".." not in name:
        kind = "audio"
    else:
        raise BundleError(f"Unexpected bundle member: {name!r}")
    if info.is_dir():
        raise BundleError(f"Bundle members must be files: {name!r}")
    mode = (info.external_attr >> 16) & 0o170000
    if mode and stat.S_ISLNK(mode):
        raise BundleError(f"Bundle members must not be links: {name!r}")
    if info.flag_bits & 0x1:
        raise BundleError("Encrypted bundles are not supported.")
    return kind


def _read_limited(zf: zipfile.ZipFile, info: zipfile.ZipInfo, limit: int) -> bytes:
    buf = io.BytesIO()
    with zf.open(info) as src:
        while True:
            chunk = src.read(_CHUNK)
            if not chunk:
                break
            if buf.tell() + len(chunk) > limit:
                raise BundleTooLarge(f"{info.filename} is larger than the bundle limit.")
            buf.write(chunk)
    return buf.getvalue()


class _Bundle:
    def __init__(self, zf: zipfile.ZipFile):
        self.zf = zf
        self.infos: dict[str, zipfile.ZipInfo] = {}
        self.manifest: dict[str, Any] = {}
        self.docs: dict[str, Any] = {}


def _open_and_validate(bundle_path: str) -> tuple[zipfile.ZipFile, _Bundle]:
    if not os.path.isfile(bundle_path):
        raise FileNotFoundError(bundle_path)
    if os.path.getsize(bundle_path) > MAX_BUNDLE_BYTES:
        raise BundleTooLarge("The bundle file is larger than the import limit.")
    try:
        zf = zipfile.ZipFile(bundle_path, "r")
    except (zipfile.BadZipFile, OSError) as exc:
        raise BundleError(f"Not a .khensu bundle (not a readable zip archive): {exc}") from exc
    try:
        b = _Bundle(zf)
        infos = zf.infolist()
        if len(infos) > MAX_MEMBERS:
            raise BundleTooLarge("The bundle has too many members.")
        total = 0
        for info in infos:
            kind = _check_member_name(info)
            key = info.filename.lower()
            if key in {k.lower() for k in b.infos}:
                raise BundleError(f"Duplicate bundle member: {info.filename!r}")
            limit = MAX_JSON_BYTES if kind == "json" else MAX_AUDIO_BYTES
            if info.file_size > limit:
                raise BundleTooLarge(f"{info.filename} is larger than the bundle limit.")
            if info.compress_size > 0 and info.file_size / info.compress_size > MAX_RATIO \
                    and info.file_size > 1024 * 1024:
                raise BundleError(f"{info.filename} has a suspicious compression ratio.")
            total += info.file_size
            if total > MAX_TOTAL_BYTES:
                raise BundleTooLarge("The bundle's contents are larger than the import limit.")
            b.infos[info.filename] = info
        if "manifest.json" not in b.infos:
            raise BundleError("The bundle has no manifest.json.")
        try:
            manifest = json.loads(_read_limited(zf, b.infos["manifest.json"], MAX_JSON_BYTES))
        except ValueError as exc:
            raise BundleError("manifest.json is not valid JSON.") from exc
        if not isinstance(manifest, dict) or manifest.get("format") != FORMAT_NAME:
            raise BundleError("manifest.json does not describe a khensu bundle.")
        fv = manifest.get("format_version")
        if not isinstance(fv, int) or fv < 1:
            raise BundleError("manifest.json has no valid format_version.")
        if fv > FORMAT_VERSION:
            raise BundleError(f"This bundle uses format version {fv}; this build reads up to "
                              f"{FORMAT_VERSION}. Update Khensu to open it.")
        b.manifest = manifest

        files = manifest.get("files") or {}
        if not isinstance(files, dict):
            raise BundleError("manifest.json 'files' must be an object.")
        for name in _JSON_MEMBERS[1:]:
            if name not in b.infos:
                b.docs[name] = {}
                continue
            data = _read_limited(zf, b.infos[name], MAX_JSON_BYTES)
            expected = (files.get(name) or {}).get("sha256") if isinstance(files.get(name), dict) else None
            if not isinstance(expected, str) or expected != _sha256_bytes(data):
                raise BundleError(f"{name} does not match the manifest hash (damaged or edited).")
            try:
                b.docs[name] = json.loads(data)
            except ValueError as exc:
                raise BundleError(f"{name} is not valid JSON.") from exc
            if not isinstance(b.docs[name], dict):
                raise BundleError(f"{name} must hold a JSON object.")

        audio = manifest.get("audio") or []
        if not isinstance(audio, list):
            raise BundleError("manifest.json 'audio' must be a list.")
        listed = set()
        for entry in audio:
            if not isinstance(entry, dict) or not isinstance(entry.get("name"), str) \
                    or not isinstance(entry.get("sha256"), str):
                raise BundleError("manifest.json has a malformed audio entry.")
            if entry["name"] not in b.infos:
                raise BundleError(f"Audio listed in the manifest is missing: {entry['name']!r}")
            listed.add(entry["name"])
        for name in b.infos:
            if name.startswith("audio/") and name not in listed:
                raise BundleError(f"Audio member not listed in the manifest: {name!r}")
        return zf, b
    except BaseException:
        zf.close()
        raise


def _validated_comments(doc: dict[str, Any]) -> list[dict[str, Any]]:
    items = doc.get("comments", [])
    if not isinstance(items, list) or len(items) > MAX_COMMENTS:
        raise BundleError("comments.json is malformed or too large.")
    out = []
    for c in items:
        if not isinstance(c, dict) or not isinstance(c.get("text"), str) \
                or not isinstance(c.get("start_s"), (int, float)) or isinstance(c.get("start_s"), bool):
            raise BundleError("comments.json has a malformed comment.")
        end = c.get("end_s")
        if end is not None and (not isinstance(end, (int, float)) or isinstance(end, bool)):
            raise BundleError("comments.json has a malformed comment end time.")
        out.append({"text": c["text"], "start_s": float(c["start_s"]),
                    "end_s": None if end is None else float(end),
                    "author": str(c.get("author", ""))[:comments_store.MAX_AUTHOR_CHARS],
                    "resolved": bool(c.get("resolved", False)),
                    "created_at": str(c.get("created_at", ""))[:40] or None})
    return out


def _validated_versions(doc: dict[str, Any]) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    items = doc.get("versions", [])
    branches = doc.get("branches", [])
    if not isinstance(items, list) or len(items) > MAX_VERSIONS or not isinstance(branches, list):
        raise BundleError("render_versions.json is malformed or too large.")
    out = []
    for v in items:
        if not isinstance(v, dict) or not isinstance(v.get("id"), str):
            raise BundleError("render_versions.json has a malformed version.")
        tags = v.get("tags", [])
        out.append({"id": v["id"][:64], "label": str(v.get("label", "")),
                    "tags": [str(t) for t in tags] if isinstance(tags, list) else [],
                    "branch": str(v.get("branch", "")), "parent_id": v.get("parent_id"),
                    "file_name": str(v.get("file_name", "")), "audio": v.get("audio"),
                    "created_at": str(v.get("created_at", ""))[:40] or None})
    bs = []
    for b in branches:
        if isinstance(b, dict) and isinstance(b.get("name"), str):
            bs.append({"name": b["name"], "base_version_id": b.get("base_version_id")})
    return out, bs


def inspect_bundle(bundle_path: str) -> dict[str, Any]:
    zf, b = _open_and_validate(bundle_path)
    try:
        comments = _validated_comments(b.docs.get("comments.json", {}))
        versions, branches = _validated_versions(b.docs.get("render_versions.json", {}))
        session = b.manifest.get("session") if isinstance(b.manifest.get("session"), dict) else {}
        return {
            "format_version": int(b.manifest["format_version"]),
            "created_at": str(b.manifest.get("created_at", "")),
            "session_name": str(session.get("name", "")),
            "branch": b.manifest.get("branch") if isinstance(b.manifest.get("branch"), str) else None,
            "comments": len(comments),
            "versions": len(versions),
            "branches": [br["name"] for br in branches],
            "audio": [{"name": e["name"], "role": str(e.get("role", "")),
                       "bytes": int(e.get("bytes", 0) or 0),
                       "original_name": str(e.get("original_name", ""))}
                      for e in b.manifest.get("audio", [])],
        }
    finally:
        zf.close()


def _extract_audio(zf: zipfile.ZipFile, info: zipfile.ZipInfo, expected_sha: str, dest: Path) -> Path:
    """Stream one audio member to dest/<basename>, counting real bytes and
    checking the hash before the file appears under its final name."""
    target = (dest / os.path.basename(info.filename)).resolve()
    if target.parent != dest.resolve():
        raise BundleError(f"Refusing to write outside the destination: {info.filename!r}")
    fd, tmp = tempfile.mkstemp(prefix=".khensu-", suffix=".part", dir=str(dest))
    written = 0
    h = hashlib.sha256()
    try:
        with os.fdopen(fd, "wb") as out, zf.open(info) as src:
            while True:
                chunk = src.read(_CHUNK)
                if not chunk:
                    break
                written += len(chunk)
                if written > MAX_AUDIO_BYTES:
                    raise BundleTooLarge(f"{info.filename} is larger than the bundle limit.")
                h.update(chunk)
                out.write(chunk)
        if h.hexdigest() != expected_sha:
            raise BundleError(f"{info.filename} does not match the manifest hash (damaged or edited).")
        if target.exists():
            if _sha256_file(str(target)) == expected_sha:
                os.remove(tmp)
                return target
            stem, ext = os.path.splitext(target.name)
            target = dest / f"{stem}_{uuid.uuid4().hex[:6]}{ext}"
        os.replace(tmp, target)
        return target
    except BaseException:
        try:
            os.remove(tmp)
        except OSError:
            pass
        raise


def import_bundle(bundle_path: str, dest_dir: str = "", session_id: str = "") -> dict[str, Any]:
    """Validate a bundle; extract its audio under `dest_dir/<bundle name>/`
    when `dest_dir` is given; attach its comments, versions and branches to
    `session_id` when given. Re-importing the same bundle is idempotent (same
    comments are not duplicated, versions are keyed by path)."""
    zf, b = _open_and_validate(bundle_path)
    try:
        comments = _validated_comments(b.docs.get("comments.json", {}))
        versions, branches = _validated_versions(b.docs.get("render_versions.json", {}))
        analysis = b.docs.get("analysis.json", {})
        session = b.manifest.get("session") if isinstance(b.manifest.get("session"), dict) else {}

        extracted: list[dict[str, Any]] = []
        member_to_path: dict[str, str] = {}
        if dest_dir:
            stem = re.sub(r"[^A-Za-z0-9 _.()\-]", "_", Path(bundle_path).stem).strip(" .") or "bundle"
            dest = Path(dest_dir) / stem[:80]
            dest.mkdir(parents=True, exist_ok=True)
            for entry in b.manifest.get("audio", []):
                p = _extract_audio(zf, b.infos[entry["name"]], entry["sha256"], dest)
                member_to_path[entry["name"]] = str(p)
                item: dict[str, Any] = {"name": entry["name"], "role": str(entry.get("role", "")),
                                        "path": str(p)}
                if isinstance(entry.get("version_id"), str):
                    item["version_id"] = entry["version_id"]
                extracted.append(item)

        comments_added = versions_added = 0
        versions_skipped = 0
        if session_id:
            existing = {(round(c["start_s"], 3), c["text"], c["author"])
                        for c in comments_store.list_comments(session_id)}
            for c in comments:
                key = (round(c["start_s"], 3), c["text"].strip(), c["author"].strip())
                if key in existing:
                    continue
                try:
                    comments_store.add_comment(session_id, c["text"], c["start_s"], c["end_s"],
                                               c["author"], c["resolved"], created_at=c["created_at"])
                except comments_store.CommentError as exc:
                    raise BundleError(f"comments.json: {exc}") from exc
                existing.add(key)
                comments_added += 1

            id_to_path = {v["id"]: member_to_path.get(v["audio"] or "", "") for v in versions}
            for name in {br["name"] for br in branches}:
                try:
                    versions_store.clean_branch(name)
                except versions_store.VersionError as exc:
                    raise BundleError(f"render_versions.json: {exc}") from exc
            for v in versions:
                path = id_to_path.get(v["id"], "")
                if not path:
                    versions_skipped += 1   # no audio came with it: nothing to restore
                    continue
                parent_path = id_to_path.get(v["parent_id"] or "", "")
                try:
                    versions_store.upsert_version(session_id, path, parent_path=parent_path,
                                                  branch=v["branch"], label=v["label"], tags=v["tags"],
                                                  created_at=v["created_at"])
                except versions_store.VersionError as exc:
                    raise BundleError(f"render_versions.json: {exc}") from exc
                versions_added += 1
            known = {br["name"] for br in versions_store.list_branches(session_id)}
            path_to_new = {v["path"]: v["id"] for v in versions_store.list_versions(session_id)}
            for br in branches:
                if br["name"] in known or not br["name"]:
                    continue
                base_path = id_to_path.get(br.get("base_version_id") or "", "")
                versions_store.create_branch(session_id, br["name"], path_to_new.get(base_path, ""))

        return {
            "format_version": int(b.manifest["format_version"]),
            "session_name": str(session.get("name", "")),
            "analysis": analysis if isinstance(analysis, dict) else {},
            "extracted": extracted,
            "comments_imported": comments_added,
            "versions_imported": versions_added,
            "versions_skipped": versions_skipped,
            "comments_in_bundle": len(comments),
            "versions_in_bundle": len(versions),
        }
    finally:
        zf.close()
