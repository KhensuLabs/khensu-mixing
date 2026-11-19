#!/usr/bin/env python3
"""Generate/update System Brain encyclopedia entries from analysis router registry.

The script introspects the router modules in local-services/khensu_services/analysis/
and merges their docstrings / registry defaults into
resources/system_brain/feature_encyclopedia.json.

Run normally to update the encyclopedia:
    python scripts/generate_system_brain_entries.py

Run with --check to fail if the encyclopedia would change (CI smoke test):
    python scripts/generate_system_brain_entries.py --check
"""
from __future__ import annotations

import argparse
import ast
import json
import re
import sys
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parent.parent
ROUTER_DIR = ROOT / "local-services" / "khensu_services" / "analysis"
ENCYCLOPEDIA_PATH = ROOT / "resources" / "system_brain" / "feature_encyclopedia.json"
REGISTRY_PATH = ROOT / "resources" / "system_brain" / "router_registry.json"


def _extract_docstring(source: str) -> str | None:
    """Return the module-level docstring from Python source, or None."""
    try:
        tree = ast.parse(source)
    except SyntaxError:
        return None
    return ast.get_docstring(tree)


def _first_sentence(text: str) -> str:
    """Return the first sentence of a docstring (up to the first period + space)."""
    text = text.strip()
    # Preserve abbreviations like "e.g." and "i.e." by a simple heuristic.
    match = re.search(r"(?<![eE]\.g|i\.e)\.[ \n]", text)
    if match:
        return text[: match.end() - 1].strip()
    return text.split("\n\n")[0].strip()


def _clean_docstring(text: str) -> str:
    """Strip leading whitespace / spec references for use as description."""
    lines = text.strip().splitlines()
    # Drop common header lines like "Spec X.Y: Feature Name (spec §N)"
    cleaned: list[str] = []
    for line in lines:
        stripped = line.strip()
        if re.match(r"^[A-Za-z][A-Za-z\s]+\(spec\s*§", stripped):
            continue
        cleaned.append(stripped)
    return "\n".join(cleaned).strip()


def _parse_router_info(source: str) -> dict[str, Any]:
    """Extract endpoint path and method from a router source file."""
    info: dict[str, Any] = {"endpoints": []}
    try:
        tree = ast.parse(source)
    except SyntaxError:
        return info

    for node in ast.walk(tree):
        if isinstance(node, ast.FunctionDef) or isinstance(node, ast.AsyncFunctionDef):
            for dec in node.decorator_list:
                call = None
                if isinstance(dec, ast.Call):
                    call = dec
                elif isinstance(dec, ast.Attribute):
                    # @router.post
                    call = None
                if call is None:
                    continue
                func = call.func
                if isinstance(func, ast.Attribute) and isinstance(func.value, ast.Name):
                    if func.value.id == "router" and func.attr in ("get", "post", "put", "delete"):
                        path = ""
                        if call.args and isinstance(call.args[0], ast.Constant):
                            path = call.args[0].value
                        info["endpoints"].append({"method": func.attr.upper(), "path": path})
    return info


def _analyzer_module_name(router_file: str) -> str | None:
    """Map router_foo.py -> foo.py if the analyzer module exists."""
    base = router_file[len("router_"):] if router_file.startswith("router_") else None
    if not base:
        return None
    candidate = ROUTER_DIR / base
    return str(candidate) if candidate.exists() else None


def _is_empty(value: Any) -> bool:
    """Treat None, empty string, and empty list as empty."""
    if value is None:
        return True
    if isinstance(value, (str, list)) and len(value) == 0:
        return True
    return False


def _merge_entry(existing: dict[str, Any], generated: dict[str, Any]) -> dict[str, Any]:
    """Preserve human-written fields; only fill empty/placeholder fields."""
    merged = dict(existing)
    for key, value in generated.items():
        if key == "id":
            continue
        if _is_empty(merged.get(key)) and not _is_empty(value):
            merged[key] = value
    return merged


def _build_entry(
    feature_id: str,
    router_file: str,
    registry_defaults: dict[str, Any],
) -> dict[str, Any]:
    if "#" in router_file:
        # ENG-5: a row of the file-analysis table (router_file_analyses.py#name);
        # its analyzer is analysis/<name>.py and its route /analyze/<name>.
        _table, name = router_file.split("#", 1)
        router_doc = None
        router_info = {"endpoints": [{"method": "POST", "path": f"/analyze/{name}"}]}
        router_file = f"router_{name}.py"
    else:
        router_path = ROUTER_DIR / router_file
        router_src = router_path.read_text(encoding="utf-8")
        router_doc = _extract_docstring(router_src)
        router_info = _parse_router_info(router_src)

    analyzer_path = _analyzer_module_name(router_file)
    analyzer_doc = ""
    if analyzer_path:
        analyzer_doc = _extract_docstring(Path(analyzer_path).read_text(encoding="utf-8")) or ""

    # Prefer analyzer docstring for descriptions, router for endpoints.
    best_doc = analyzer_doc or router_doc or ""
    best_doc = _clean_docstring(best_doc)

    short = _first_sentence(best_doc) if best_doc else f"{feature_id} analysis endpoint."
    detailed = best_doc if best_doc else ""

    endpoints = router_info.get("endpoints", [])
    endpoint_note = ""
    if endpoints:
        parts = [f"{e['method']} {e['path']}" for e in endpoints if e.get("path")]
        if parts:
            endpoint_note = "Endpoint(s): " + ", ".join(parts) + "."
    if endpoint_note and detailed and not detailed.endswith(endpoint_note):
        detailed = detailed.rstrip(".") + ".\n\n" + endpoint_note
    elif endpoint_note and not detailed:
        detailed = endpoint_note

    name = registry_defaults.get("name") or feature_id.replace("_", " ").title()
    return {
        "id": feature_id,
        "name": name,
        "category": registry_defaults.get("category", "Analysis"),
        "short_description": short,
        "detailed_description": detailed,
        "logic_and_math": "",
        "why_it_exists": "",
        "when_to_use": "",
        "inputs": [],
        "tags": registry_defaults.get("tags", ["technical", "analysis"]),
        "desktop_location": registry_defaults.get("panel_location", ""),
        "plugin_available": False,
        "related_features": [],
    }


def generate(
    *,
    check: bool = False,
    encyclopedia_path: Path = ENCYCLOPEDIA_PATH,
    registry_path: Path = REGISTRY_PATH,
) -> bool:
    """Generate/update encyclopedia entries.

    Returns True if the encyclopedia was modified (or would be modified when
    check=True); False if no changes were needed.
    """
    registry = json.loads(registry_path.read_text(encoding="utf-8"))
    encyclopedia = json.loads(encyclopedia_path.read_text(encoding="utf-8"))

    existing_by_id = {entry["id"]: entry for entry in encyclopedia.get("features", [])}
    modified = False

    for router_file, meta in registry.get("routers", {}).items():
        feature_id = meta["feature_id"]
        generated = _build_entry(feature_id, router_file, meta)

        if feature_id in existing_by_id:
            merged = _merge_entry(existing_by_id[feature_id], generated)
            if merged != existing_by_id[feature_id]:
                existing_by_id[feature_id] = merged
                modified = True
                print(f"Updated: {feature_id}")
        else:
            existing_by_id[feature_id] = generated
            encyclopedia["features"].append(generated)
            modified = True
            print(f"Added:   {feature_id}")

    if not modified:
        print("Encyclopedia is already in sync with router registry.")
        return False

    if check:
        print("Encyclopedia is out of sync with router registry.")
        return True

    # Preserve insertion order; append new entries at the end.
    encyclopedia_path.write_text(
        json.dumps(encyclopedia, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    print(f"Wrote {encyclopedia_path}")
    return True


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--check",
        action="store_true",
        help="Fail with exit code 1 if the encyclopedia would change.",
    )
    args = parser.parse_args(argv)

    changed = generate(check=args.check)
    return 1 if (args.check and changed) else 0


if __name__ == "__main__":
    sys.exit(main())
