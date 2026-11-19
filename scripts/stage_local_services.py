#!/usr/bin/env python3
"""Stage the Python service for the installer (audit P0-3).

The installer used to copy ``local-services\\*`` recursively with an exclude
list that matched almost nothing: test scratch (``.pytest_tmp_*``, several
hundred MB each), ``tests/``, caches, ``*.egg-info`` and development lock
files would all have shipped. This stages from ``git ls-files`` through an
allow-list instead, so an untracked or unlisted file can never reach a user:

    python scripts/stage_local_services.py            # stage into build/packaging/local-services
    python scripts/stage_local_services.py --check    # stage, then smoke-check the staged tree
    python scripts/stage_local_services.py --check-only DIR

``packaging/khensu-installer.iss`` packages the staged directory. The smoke
check fails on a missing entry point, anything matching the forbidden
patterns, a file that does not compile, or a tree over the size budget.
"""
from __future__ import annotations

import argparse
import fnmatch
import hashlib
import json
import shutil
import subprocess
import sys
from pathlib import Path, PurePosixPath

ROOT = Path(__file__).resolve().parents[1]
SOURCE = "local-services"
DEFAULT_OUT = ROOT / "build" / "packaging" / "local-services"

# Paths are relative to local-services/, POSIX style, as git prints them.
ALLOW = [
    "khensu_services/**",           # the service package (code, templates, assets, learned models)
    "launch_service.py",            # the entry point the app spawns
    "pyproject.toml",               # `pip install -e .` on a first-run bootstrap
    "requirements-*-hashed.txt",    # hash-pinned runtime + optional stacks
    "demucs-stems-lock.json",       # the reviewed stems profile (install-optional-stack.ps1)
    "runtime-sbom.cdx.json",        # the SBOM of what ships (CE-ENG-2)
]
# Never shipped, even if a future allow-list entry would match them.
FORBIDDEN = [
    "tests/**", "*/tests/**", ".pytest*", "*/.pytest*", "**/__pycache__/**", "*.pyc", ".ruff_cache*",
    "*.egg-info*", ".venv*", "*/.venv*", "desktop.ini", "*/desktop.ini", "requirements.txt",
    "requirements-lock.txt", "uv.lock", "scripts/**", "test_output.txt",
    ".env", "*.env", "*.pem", "*.key", "*.pfx", "token.txt", "settings.json", "*.db", "*.sqlite*",
]
REQUIRED = ["launch_service.py", "pyproject.toml", "requirements-runtime-hashed.txt",
            "khensu_services/__init__.py", "khensu_services/server.py"]
SIZE_BUDGET_MB = 60
# Bundled learned models are trained locally and git-ignored, so they come
# from the working tree, not git: every file in resources/models whose model
# name is in the repo-controlled registry (artifact + .json / .card sidecars).
# The loader refuses a bundled model that doesn't match its pin in
# model_pins.json, so the check refuses to stage one too.
MODELS_DIR = "resources/models"
REGISTRY = "khensu_services/analysis/model_registry.json"
PINS = "khensu_services/analysis/model_pins.json"


def _model_name(filename: str) -> str:
    return filename.split(".", 1)[0]


def _registry_names(service_root: Path) -> set[str]:
    try:
        return set(json.loads((service_root / REGISTRY).read_text(encoding="utf-8")).get("models", {}))
    except (OSError, ValueError, AttributeError):
        return set()


def _pins(service_root: Path) -> dict[str, str]:
    try:
        pins = json.loads((service_root / PINS).read_text(encoding="utf-8")).get("sha256", {})
        return {str(k): str(v).lower() for k, v in pins.items() if isinstance(v, str) and len(v) == 64}
    except (OSError, ValueError, AttributeError):
        return {}


def bundled_models() -> list[str]:
    models = ROOT / SOURCE / MODELS_DIR
    names = _registry_names(ROOT / SOURCE)
    if not models.is_dir():
        return []
    return sorted(f"{MODELS_DIR}/{p.name}" for p in models.iterdir()
                  if p.is_file() and _model_name(p.name) in names)


def _matches(rel: str, patterns: list[str]) -> bool:
    path = PurePosixPath(rel)
    for pattern in patterns:
        if pattern.endswith("/**"):
            prefix = pattern[:-3]
            if fnmatch.fnmatch(rel, prefix + "/*") or any(
                    fnmatch.fnmatch(str(PurePosixPath(*path.parts[:i])), prefix)
                    for i in range(1, len(path.parts))):
                return True
        elif fnmatch.fnmatch(rel, pattern) or fnmatch.fnmatch(path.name, pattern):
            return True
    return False


def tracked_files() -> list[str]:
    out = subprocess.run(["git", "ls-files", "-z", "--", SOURCE], cwd=ROOT, check=True,
                         capture_output=True).stdout.decode("utf-8")
    prefix = SOURCE + "/"
    return [p[len(prefix):] for p in out.split("\0") if p.startswith(prefix)]


def select(files: list[str]) -> list[str]:
    return sorted(f for f in files if _matches(f, ALLOW) and not _matches(f, FORBIDDEN))


def stage(out: Path) -> int:
    files = select(tracked_files()) + bundled_models()
    if out.exists():
        shutil.rmtree(out)
    for rel in files:
        src = ROOT / SOURCE / rel
        if not src.is_file():  # tracked but deleted in the working tree
            continue
        dst = out / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)
    return len(files)


def check(out: Path) -> list[str]:
    """Smoke-checks a staged tree; returns the problems found (empty = OK)."""
    if not out.is_dir():
        return [f"{out} does not exist - stage first"]
    problems = [f"missing {req}" for req in REQUIRED if not (out / req).is_file()]
    total = 0
    for path in out.rglob("*"):
        rel = path.relative_to(out).as_posix()
        if path.is_file():
            total += path.stat().st_size
        if _matches(rel, FORBIDDEN):
            problems.append(f"forbidden: {rel}")
    if total > SIZE_BUDGET_MB * 1024 * 1024:
        problems.append(f"{total / 1048576:.1f} MB staged, over the {SIZE_BUDGET_MB} MB budget")
    if not _compiles(out):
        problems.append("khensu_services does not compile")
    # v1.0.0: no secrets or personal data (keys, tokens, the builder's email
    # or home path) - scripts/scan_release_secrets.py.
    sys.path.insert(0, str(ROOT / "scripts"))
    from scan_release_secrets import scan as _scan_secrets

    problems.extend(f"secret scan: {hit}" for hit in _scan_secrets([out]))
    pins = _pins(out)
    models = out / MODELS_DIR
    if models.is_dir():
        for path in sorted(models.iterdir()):
            pin = pins.get(_model_name(path.name)) if path.suffix in (".onnx", ".npz", ".pkl", ".joblib") else None
            if pin and hashlib.sha256(path.read_bytes()).hexdigest() != pin:
                problems.append(f"{MODELS_DIR}/{path.name} does not match its model_pins.json SHA-256")
    return problems


def _compiles(out: Path) -> bool:
    """Compiles every staged .py in memory, so no __pycache__ lands in the tree."""
    ok = True
    entry = [out / "launch_service.py"] if (out / "launch_service.py").is_file() else []  # missing: reported above
    for path in sorted((out / "khensu_services").rglob("*.py")) + entry:
        try:
            compile(path.read_text(encoding="utf-8"), str(path), "exec")
        except (SyntaxError, UnicodeDecodeError) as exc:
            print(f"  does not compile: {path.relative_to(out)}: {exc}", file=sys.stderr)
            ok = False
    return ok


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Stage the Python service for the installer (audit P0-3).")
    ap.add_argument("--out", type=Path, default=DEFAULT_OUT)
    ap.add_argument("--check", action="store_true", help="stage, then smoke-check the staged tree")
    ap.add_argument("--check-only", type=Path, metavar="DIR", help="smoke-check an already staged tree")
    args = ap.parse_args(argv)

    if args.check_only:
        out = args.check_only
    else:
        out = args.out
        n = stage(out)
        print(f"staged {n} files into {out}")
    if args.check or args.check_only:
        problems = check(out)
        for p in problems:
            print("  " + p, file=sys.stderr)
        if problems:
            print(f"installer stage check: FAILED ({len(problems)} problem(s))", file=sys.stderr)
            return 1
        print("installer stage check: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
