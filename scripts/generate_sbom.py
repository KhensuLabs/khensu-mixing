#!/usr/bin/env python3
"""Generate a CycloneDX 1.5 SBOM for the suite (CE-ENG-2).

Components come from two places:
- every package in ``local-services/uv.lock`` (name, version, the sdist/wheel
  hashes it pins), as ``pkg:pypi`` purls;
- every ``FetchContent_Declare`` in ``cmake/*.cmake`` and the component
  ``CMakeLists.txt`` files (repository + pinned tag/commit), as ``pkg:github``
  purls when the repository is on GitHub.

Stdlib only, deterministic output (sorted components, no timestamp unless
``--timestamp``), so the committed or released SBOM diffs cleanly.

    python scripts/generate_sbom.py --out .build-artifacts/sbom/khensu.cdx.json
"""
from __future__ import annotations

import argparse
import json
import re
import sys
import tomllib
import uuid
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CMAKE_SOURCES = ("cmake", "CMakeLists.txt", "shared-core/CMakeLists.txt",
                 "desktop-app/CMakeLists.txt", "plugin-copilot/CMakeLists.txt",
                 "tests/CMakeLists.txt")

_DECLARE_RE = re.compile(r"FetchContent_Declare\s*\((.*?)\)", re.S | re.I)


def _product_version(root: Path) -> str:
    header = root / "shared-core/include/khensu/common/Version.h"
    if header.exists():
        m = re.search(r'"(\d+\.\d+\.\d+)"', header.read_text(encoding="utf-8"))
        if m:
            return m.group(1)
    return "0.0.0"


def python_components(lock_path: Path) -> list[dict]:
    data = tomllib.loads(lock_path.read_text(encoding="utf-8"))
    out: list[dict] = []
    for pkg in data.get("package", []):
        name, version = pkg.get("name"), pkg.get("version")
        if not name or not version:
            continue
        source = pkg.get("source", {})
        if "editable" in source or "virtual" in source:
            continue  # the project itself
        comp = {
            "type": "library",
            "bom-ref": f"pypi:{name}@{version}",
            "name": name,
            "version": version,
            "purl": f"pkg:pypi/{name.lower()}@{version}",
        }
        hashes = []
        for art in ([pkg["sdist"]] if "sdist" in pkg else []) + pkg.get("wheels", []):
            h = art.get("hash", "")
            if h.startswith("sha256:"):
                hashes.append(h.split(":", 1)[1])
        if hashes:
            # One hash per component keeps the file small; all artifacts of
            # a release share a version, and the hashed requirement exports
            # carry the full set.
            comp["hashes"] = [{"alg": "SHA-256", "content": sorted(hashes)[0]}]
        out.append(comp)
    return out


def _cmake_files(root: Path) -> list[Path]:
    files: list[Path] = []
    for rel in CMAKE_SOURCES:
        p = root / rel
        if p.is_dir():
            files.extend(sorted(p.glob("*.cmake")))
        elif p.exists():
            files.append(p)
    return files


def cmake_components(root: Path) -> list[dict]:
    out: dict[str, dict] = {}
    for path in _cmake_files(root):
        text = re.sub(r"#[^\n]*", "", path.read_text(encoding="utf-8"))
        for block in _DECLARE_RE.findall(text):
            tokens = block.split()
            if not tokens:
                continue
            name = tokens[0]
            fields = {}
            for key in ("GIT_REPOSITORY", "GIT_TAG", "URL", "URL_HASH"):
                if key in tokens:
                    i = tokens.index(key)
                    if i + 1 < len(tokens):
                        fields[key] = tokens[i + 1].strip('"')
            repo = fields.get("GIT_REPOSITORY") or fields.get("URL", "")
            version = fields.get("GIT_TAG", "")
            comp = {"type": "library", "bom-ref": f"cmake:{name}", "name": name,
                    "version": version or "unpinned"}
            m = re.match(r"https://github\.com/([^/]+)/([^/.]+)", repo)
            if m and version:
                comp["purl"] = f"pkg:github/{m.group(1)}/{m.group(2)}@{version}"
            if repo:
                comp["externalReferences"] = [{"type": "vcs", "url": repo}]
            out[name] = comp
    return [out[k] for k in sorted(out)]


def build_sbom(root: Path, *, timestamp: bool = False) -> dict:
    components = python_components(root / "local-services/uv.lock") + cmake_components(root)
    components.sort(key=lambda c: c["bom-ref"])
    version = _product_version(root)
    metadata: dict = {
        "component": {"type": "application", "bom-ref": "khensu-mixing",
                      "name": "Khensu Mixing & Mastering Suite", "version": version},
        "tools": {"components": [{"type": "application", "name": "generate_sbom.py"}]},
    }
    if timestamp:
        metadata["timestamp"] = datetime.now(timezone.utc).isoformat(timespec="seconds")
    # Deterministic serial: derived from the component set, so an unchanged
    # dependency tree yields a byte-identical SBOM.
    digest_src = json.dumps(components, sort_keys=True)
    serial = uuid.uuid5(uuid.NAMESPACE_URL, "khensu-sbom:" + digest_src)
    return {"bomFormat": "CycloneDX", "specVersion": "1.5",
            "serialNumber": f"urn:uuid:{serial}", "version": 1,
            "metadata": metadata, "components": components}


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Generate a CycloneDX 1.5 SBOM for the suite.")
    ap.add_argument("--out", default=str(ROOT / ".build-artifacts/sbom/khensu.cdx.json"))
    ap.add_argument("--timestamp", action="store_true", help="stamp metadata.timestamp")
    args = ap.parse_args(argv)
    sbom = build_sbom(ROOT, timestamp=args.timestamp)
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(sbom, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"wrote {out} ({len(sbom['components'])} components)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
