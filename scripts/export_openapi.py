#!/usr/bin/env python3
"""Export the service's OpenAPI schema to resources/api/openapi.json.

The committed schema is the contract the C++ side is generated from
(scripts/gen_cpp_api.py -> shared-core/include/khensu/ipc/generated/). It is
written deterministically (sorted keys, two-space indent, LF, trailing newline)
and without the service version, so it only changes when a route or a
request/response model changes - not on every SERVICE_VERSION bump.

Usage (repo root):
    python scripts/export_openapi.py          # rewrite the file
    python scripts/export_openapi.py --check  # exit 1 if the file is stale
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SERVICE_ROOT = ROOT / "local-services"
OUT = ROOT / "resources" / "api" / "openapi.json"


def build_schema() -> dict:
    if str(SERVICE_ROOT) not in sys.path:
        sys.path.insert(0, str(SERVICE_ROOT))
    from khensu_services.server import app

    schema = app.openapi()
    # Volatile: bumped on every release; the version handshake is checked
    # separately by check_doc_drift.py.
    schema.get("info", {}).pop("version", None)
    return schema


def render(schema: dict) -> str:
    return json.dumps(schema, indent=2, sort_keys=True, ensure_ascii=True) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--check", action="store_true", help="fail if the committed file is stale")
    args = parser.parse_args()

    text = render(build_schema())
    if args.check:
        current = OUT.read_text(encoding="utf-8") if OUT.exists() else ""
        if current.replace("\r\n", "\n") != text:
            print(f"{OUT.relative_to(ROOT)} is stale - run `python scripts/export_openapi.py` "
                  "then `python scripts/gen_cpp_api.py`.")
            return 1
        return 0

    OUT.parent.mkdir(parents=True, exist_ok=True)
    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    print(f"wrote {OUT.relative_to(ROOT)} ({len(schema_paths(text))} paths)")
    return 0


def schema_paths(text: str) -> dict:
    return json.loads(text).get("paths", {})


if __name__ == "__main__":
    sys.exit(main())
