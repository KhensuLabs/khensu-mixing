"""Compare the installed Python environment against uv.lock (PKG-2).

Local runs are the only verification while hosted CI is blocked (CFG-01), so
a dev ``.venv`` that drifted from the lock tests a different stack from the
one that ships. This reads ``local-services/requirements-hashed.txt`` (the
base + dev export of ``uv.lock``, kept in sync by
``scripts/generate_hashed_requirements.py`` and the doc-drift gate), evaluates
each line's environment marker for *this* interpreter, and compares the pinned
version with what ``importlib.metadata`` reports.

Run it with the interpreter you want to check:

    local-services/.venv/Scripts/python.exe scripts/check_venv_lock_sync.py

Exit codes: 0 in sync, 1 drift found (mismatches listed), 2 could not check.
``scripts/verify_all.ps1`` runs it as the non-blocking ``lock-sync`` step and
turns drift into a WARN, not a failure. Optional extras are not checked: they
are installed per machine through their own hashed exports.
"""
from __future__ import annotations

import argparse
import re
import sys
from importlib import metadata
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_EXPORT = ROOT / "local-services" / "requirements-hashed.txt"

_PIN = re.compile(r"^([A-Za-z0-9][A-Za-z0-9._-]*)(\[[^\]]*\])?==([^\s;\\]+)\s*(?:;\s*(.*?))?\s*\\?$")


def _canon(name: str) -> str:
    return re.sub(r"[-_.]+", "-", name).lower()


def _marker_applies(marker: str | None) -> bool:
    if not marker:
        return True
    try:
        from packaging.markers import Marker
    except ImportError:  # pragma: no cover - packaging ships with pip's vendored copy only
        return True
    return Marker(marker).evaluate()


def locked_pins(export: Path) -> dict[str, str]:
    pins: dict[str, str] = {}
    for raw in export.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith(("#", "--")):
            continue
        m = _PIN.match(line)
        if not m:
            continue
        name, _extras, version, marker = m.groups()
        if _marker_applies(marker):
            pins[_canon(name)] = version
    return pins


def installed_versions() -> dict[str, str]:
    out: dict[str, str] = {}
    for dist in metadata.distributions():
        name = dist.metadata["Name"]
        if name:
            out.setdefault(_canon(name), dist.version)
    return out


def _same(locked: str, installed: str) -> bool:
    try:
        from packaging.version import Version
        return Version(locked) == Version(installed)
    except Exception:
        return locked == installed


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--export", type=Path, default=DEFAULT_EXPORT,
                        help="lock-derived requirements export to compare against")
    args = parser.parse_args()

    if not args.export.exists():
        print(f"lock-sync: {args.export} not found", file=sys.stderr)
        return 2
    pins = locked_pins(args.export)
    if not pins:
        print(f"lock-sync: no pins parsed from {args.export}", file=sys.stderr)
        return 2
    have = installed_versions()

    missing = sorted(n for n in pins if n not in have)
    differ = sorted((n, pins[n], have[n]) for n in pins if n in have and not _same(pins[n], have[n]))

    print(f"lock-sync: {sys.executable}")
    print(f"lock-sync: {len(pins)} base+dev pins from {args.export.name}; "
          f"{len(differ)} differ, {len(missing)} not installed")
    if differ:
        width = max(len(n) for n, _, _ in differ)
        print("  package".ljust(width + 2) + "  locked -> installed")
        for name, locked, installed in differ:
            print(f"  {name.ljust(width)}  {locked} -> {installed}")
    for name in missing:
        print(f"  {name}  {pins[name]} -> (not installed)")
    if differ or missing:
        print("lock-sync: to match the lock: pip install --require-hashes -r "
              "local-services/requirements-hashed.txt (this changes the shared .venv)")
        return 1
    print("lock-sync: OK - the environment matches uv.lock")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
