"""Validate a provenance-backed corpus manifest before model work.

Usage:
    python scripts/validate_evaluation_corpus.py path/to/manifest.json

The command intentionally validates only metadata/governance. It does not
train a model and does not make a classifier available.
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "local-services"))

from khensu_services.evaluation.corpus_schema import CorpusItem, validate_split  # noqa: E402


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print("Usage: validate_evaluation_corpus.py MANIFEST.json", file=sys.stderr)
        return 2
    data = json.loads(Path(argv[1]).read_text(encoding="utf-8"))
    items = [CorpusItem(**item) for item in data.get("items", [])]
    result = validate_split(items)
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
