#!/usr/bin/env python3
"""Generate the service's copy of the migration chain (ENG-8).

The desktop app compiles storage/schema/NNN_*.sql into SessionManager
(cmake/GenerateMigrations.cmake). The service may open khensu.db first, so its
stores create their tables too - and they used to keep hand-written copies of
that DDL. This writes the same .sql files into
``local-services/khensu_services/common/schema_sql.py`` so the stores run the
migrations themselves and there is no second copy to drift.

    python scripts/gen_schema_sql.py            # write
    python scripts/gen_schema_sql.py --check    # exit 1 when stale (doc drift)
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCHEMA = ROOT / "storage" / "schema"
OUT = ROOT / "local-services" / "khensu_services" / "common" / "schema_sql.py"


def render() -> str:
    files = sorted(SCHEMA.glob("[0-9][0-9][0-9]_*.sql"))
    lines = [
        '"""The migration chain, generated from storage/schema/NNN_*.sql (ENG-8).',
        "",
        "Do not edit: run ``python scripts/gen_schema_sql.py`` after changing a",
        "migration. The service's stores run the migrations their tables come",
        "from (all written ``IF NOT EXISTS``) instead of keeping their own DDL.",
        '"""',
        "",
        "MIGRATIONS: dict[int, str] = {",
    ]
    for f in files:
        version = int(re.match(r"(\d{3})_", f.name).group(1))
        sql = f.read_text(encoding="utf-8").replace("\r\n", "\n")
        assert '"""' not in sql, f"{f.name} contains a triple quote"
        lines.append(f'    {version}: """{sql}""",  # {f.name}')
    lines += ["}", ""]
    return "\n".join(lines)


def main() -> int:
    text = render()
    if "--check" in sys.argv:
        current = OUT.read_text(encoding="utf-8") if OUT.exists() else ""
        if current != text:
            print(f"{OUT.relative_to(ROOT)} is stale: run python scripts/gen_schema_sql.py")
            return 1
        print("OK: schema_sql.py matches storage/schema")
        return 0
    OUT.write_text(text, encoding="utf-8", newline="\n")
    print(f"wrote {OUT.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
