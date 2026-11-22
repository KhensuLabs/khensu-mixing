"""Which locked packages of an export have no wheel for a platform.

The desktop's first run installs ``requirements-runtime-hashed.txt`` with
``--require-hashes``: a package with no wheel for the machine falls back to
its sdist and has to compile, which fails on a user's Mac (no compiler, or a
native toolchain like LLVM for llvmlite). This lists, per Python version, the
runtime packages ``uv.lock`` has no wheel for on the platform asked:

    python scripts/check_platform_wheels.py --platform macos-x86_64
    python scripts/check_platform_wheels.py --platform macos-arm64 --export requirements-runtime-hashed.txt

Exit 1 when a package lacks a wheel (pure-Python ``none-any`` wheels count).
"""
from __future__ import annotations

import argparse
import re
import sys
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SERVICES = ROOT / "local-services"
PLATFORMS = {
    "macos-x86_64": ("macosx", ("x86_64", "universal2")),
    "macos-arm64": ("macosx", ("arm64", "universal2")),
    "win-amd64": ("win", ("amd64",)),
}


def requirement_pins(export: Path) -> list[tuple[str, str, str]]:
    """(name, version, environment marker) of each pinned line."""
    out = []
    for line in export.read_text(encoding="utf-8").splitlines():
        m = re.match(r"^([A-Za-z0-9_.\-]+)==([^\s;\\]+)\s*(?:;\s*([^\\]+))?", line)
        if m:
            out.append((m.group(1).lower().replace("_", "-"), m.group(2), (m.group(3) or "").strip()))
    return out


def wheels_for(lock: dict, name: str, version: str) -> list[str]:
    return [w["url"].rsplit("/", 1)[-1] for p in lock["package"]
            if p["name"] == name and p["version"] == version for w in p.get("wheels", [])]


def has_wheel(wheels: list[str], os_tag: str, archs: tuple[str, ...], cp: int) -> bool:
    for w in wheels:
        if w.endswith("-none-any.whl"):
            return True
        tags = w[:-4].split("-")[-3:]
        py, abi, plat = tags if len(tags) == 3 else ("", "", "")
        if os_tag not in plat or not any(a in plat for a in archs):
            continue
        if f"cp{cp}" in py.split(".") or (abi == "abi3" and any(
                t.startswith("cp3") and int(t[3:] or 0) <= cp for t in py.split("."))) or py.startswith("py3"):
            return True
    return False


def marker_applies(marker: str, platform: str) -> bool:
    """Rough: drop lines whose marker names another OS / machine."""
    m = marker.replace('"', "'")
    if "sys_platform == 'win32'" in m and not platform.startswith("win"):
        return False
    if "sys_platform != 'win32'" in m and platform.startswith("win"):
        return False
    if "sys_platform == 'darwin'" in m and not platform.startswith("macos"):
        return False
    if "sys_platform != 'darwin'" in m and platform.startswith("macos"):
        return False
    if "platform_machine == 'x86_64'" in m and not platform.endswith("x86_64"):
        return False
    if "platform_machine != 'x86_64'" in m and platform.endswith("x86_64"):
        return False
    if "platform_machine == 'arm64'" in m and not platform.endswith("arm64"):
        return False
    return True


def _python_marker_applies(marker: str, cp: int) -> bool:
    """python_full_version / python_version comparisons against 3.<cp-300>."""
    ver = (3, cp - 300)
    for op, major, minor in re.findall(r"python_(?:full_)?version\s*([<>=!]=?)\s*'(\d+)\.(\d+)", marker):
        want = (int(major), int(minor))
        ok = {"<": ver < want, "<=": ver <= want, ">": ver > want, ">=": ver >= want,
              "==": ver == want, "!=": ver != want}[op]
        if not ok:
            return False
    return True


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--platform", choices=sorted(PLATFORMS), required=True)
    ap.add_argument("--export", default="requirements-runtime-hashed.txt")
    ap.add_argument("--pythons", default="311,312,313")
    a = ap.parse_args()
    lock = tomllib.loads((SERVICES / "uv.lock").read_text(encoding="utf-8"))
    os_tag, archs = PLATFORMS[a.platform]
    missing = 0
    for cp in (int(x) for x in a.pythons.split(",")):
        lacking = []
        for name, version, marker in requirement_pins(SERVICES / a.export):
            if marker and not marker_applies(marker, a.platform):
                continue
            if not _python_marker_applies(marker, cp):
                continue
            wheels = wheels_for(lock, name, version)
            if wheels and not has_wheel(wheels, os_tag, archs, cp):
                lacking.append(f"{name}=={version}")
        print(f"{a.platform} cp{cp}: " + (", ".join(lacking) if lacking else "every package has a wheel"))
        missing += len(lacking)
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
