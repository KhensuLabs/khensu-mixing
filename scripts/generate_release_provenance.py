#!/usr/bin/env python3
"""Generate a release provenance manifest for built desktop/plugin artifacts.

The manifest is intentionally simple JSON: a deterministic inventory of the
release subjects (files or directory bundles such as .vst3), the reviewed
materials that fed them, and the CI/build metadata available in the current
environment.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from collections.abc import Iterable
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUF_SIZE = 1 << 20


@dataclass(frozen=True)
class FileDigest:
    rel_path: str
    sha256: str
    size_bytes: int


def _repo_relative(path: Path, *, root: Path) -> str:
    resolved = path.resolve()
    try:
        return resolved.relative_to(root.resolve()).as_posix()
    except ValueError:
        return resolved.as_posix()


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(BUF_SIZE), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _walk_directory(path: Path, *, root: Path) -> list[FileDigest]:
    files: list[FileDigest] = []
    for child in sorted(p for p in path.rglob("*") if p.is_file()):
        files.append(
            FileDigest(
                rel_path=_repo_relative(child, root=root),
                sha256=_sha256_file(child),
                size_bytes=child.stat().st_size,
            )
        )
    return files


def _tree_sha256(entries: Iterable[FileDigest]) -> str:
    digest = hashlib.sha256()
    for entry in entries:
        digest.update(entry.rel_path.encode("utf-8"))
        digest.update(b"\0")
        digest.update(entry.sha256.encode("ascii"))
        digest.update(b"\0")
        digest.update(str(entry.size_bytes).encode("ascii"))
        digest.update(b"\n")
    return digest.hexdigest()


def describe_path(path: Path, *, root: Path) -> dict:
    if not path.exists():
        raise FileNotFoundError(f"Missing path for provenance manifest: {path}")

    if path.is_file():
        return {
            "path": _repo_relative(path, root=root),
            "kind": "file",
            "sha256": _sha256_file(path),
            "size_bytes": path.stat().st_size,
        }

    if path.is_dir():
        entries = _walk_directory(path, root=root)
        return {
            "path": _repo_relative(path, root=root),
            "kind": "directory",
            "sha256_tree": _tree_sha256(entries),
            "file_count": len(entries),
            "entries": [
                {"path": entry.rel_path, "sha256": entry.sha256, "size_bytes": entry.size_bytes}
                for entry in entries
            ],
        }

    raise ValueError(f"Unsupported path type for provenance manifest: {path}")


def build_manifest(
    *,
    artifacts: list[Path],
    materials: list[Path],
    generated_at_utc: str | None = None,
    env: dict[str, str] | None = None,
    root: Path = ROOT,
) -> dict:
    env = dict(os.environ if env is None else env)
    generated_at_utc = generated_at_utc or datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")

    builder = {
        key.lower(): env[key]
        for key in (
            "GITHUB_SHA",
            "GITHUB_REF",
            "GITHUB_RUN_ID",
            "GITHUB_RUN_ATTEMPT",
            "GITHUB_WORKFLOW",
            "GITHUB_JOB",
            "GITHUB_REPOSITORY",
            "GITHUB_ACTOR",
        )
        if env.get(key)
    }

    return {
        "schema_version": 1,
        "generated_at_utc": generated_at_utc,
        "builder": builder,
        "subjects": [describe_path(path, root=root) for path in artifacts],
        "materials": [describe_path(path, root=root) for path in materials],
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--artifact", action="append", required=True, help="built release artifact file or directory")
    parser.add_argument("--material", action="append", default=[], help="reviewed input file or directory to record")
    parser.add_argument("--output", required=True, help="path to write the JSON manifest")
    parser.add_argument("--generated-at", default=None, help="override generated_at_utc (ISO-8601) for testing")
    args = parser.parse_args()

    manifest = build_manifest(
        artifacts=[Path(p) for p in args.artifact],
        materials=[Path(p) for p in args.material],
        generated_at_utc=args.generated_at,
    )

    out = Path(args.output)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"wrote {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
