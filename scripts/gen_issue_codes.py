#!/usr/bin/env python3
"""Generate the service's copy of the finding-code table (ENG-7).

The C++ recommenders own the finding codes: they are listed once, in
shared-core/include/khensu/recommendations/IssueCodes.h (``kIssueCodes``).
This writes the same table to
``local-services/khensu_services/recommendations/issue_codes.py`` so the
Python side - which only words findings - reads the codes and their taste flag
from it instead of keeping its own list.

    python scripts/gen_issue_codes.py            # write
    python scripts/gen_issue_codes.py --check    # exit 1 when stale (doc drift)
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "shared-core" / "include" / "khensu" / "recommendations" / "IssueCodes.h"
SPECTRAL = ROOT / "shared-core" / "include" / "khensu" / "recommendations" / "SpectralRecommender.h"
#: Engine constants the service repeats (FIX-9): name in the C++ header -> Python name.
CONSTANTS = {"kCentroidCeilingRatio": "CENTROID_CEILING_RATIO", "kCentroidFloorRatio": "CENTROID_FLOOR_RATIO"}
OUT = ROOT / "local-services" / "khensu_services" / "recommendations" / "issue_codes.py"

_ROW = re.compile(r'\{\s*"([a-z][a-z0-9_]*)"\s*,\s*(true|false)\s*,\s*IssueOrigin::(Engine|Service|Legacy)\s*\}')


def parse() -> list[tuple[str, bool, str]]:
    text = HEADER.read_text(encoding="utf-8")
    block = text[text.index("kIssueCodes[]"):text.index("};", text.index("kIssueCodes[]"))]
    rows = [(c, t == "true", o.lower()) for c, t, o in _ROW.findall(block)]
    assert rows, "no rows parsed from IssueCodes.h"
    assert len({c for c, _, _ in rows}) == len(rows), "duplicate code in IssueCodes.h"
    return rows


def constants() -> dict[str, float]:
    text = SPECTRAL.read_text(encoding="utf-8")
    out = {}
    for cpp, py in CONSTANTS.items():
        m = re.search(rf"{cpp}\s*=\s*([0-9.]+)f", text)
        assert m, f"{cpp} not found in SpectralRecommender.h"
        out[py] = float(m.group(1))
    return out


def render() -> str:
    lines = [
        '"""Finding codes, generated from shared-core/.../recommendations/IssueCodes.h (ENG-7).',
        "",
        "Do not edit: change the C++ table and run ``python scripts/gen_issue_codes.py``.",
        "The C++ recommenders own the codes and their severity; the service words them.",
        '"""',
        "",
        "# code -> (taste decision, origin: engine | service | legacy)",
        "ISSUE_CODES: dict[str, tuple[bool, str]] = {",
    ]
    for code, taste, origin in parse():
        lines.append(f'    "{code}": ({taste}, "{origin}"),')
    lines += [
        "}",
        "",
        "TASTE_CODES: frozenset[str] = frozenset(c for c, (taste, _) in ISSUE_CODES.items() if taste)",
        "",
        "# Engine constants the service repeats (FIX-9), from SpectralRecommender.h:",
        "# the centroid window is the genre centroid times these.",
    ]
    for name, value in constants().items():
        lines.append(f"{name} = {value!r}")
    lines.append("")
    return "\n".join(lines)


def main() -> int:
    text = render()
    if "--check" in sys.argv:
        current = OUT.read_text(encoding="utf-8") if OUT.exists() else ""
        if current != text:
            print(f"{OUT.relative_to(ROOT)} is stale: run python scripts/gen_issue_codes.py")
            return 1
        print("OK: issue_codes.py matches IssueCodes.h")
        return 0
    OUT.write_text(text, encoding="utf-8", newline="\n")
    print(f"wrote {OUT.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
