"""FULL-4: MaskingRecommender's per-pair bars are the ones the corpus run
measured (scripts/eval_masking.py -> bench-results/masking_eval.json)."""
import json
import re
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
HEADER = ROOT / "shared-core" / "include" / "khensu" / "recommendations" / "MaskingRecommender.h"
EVAL = ROOT / "bench-results" / "masking_eval.json"


def _header_bars() -> dict[str, float]:
    text = HEADER.read_text(encoding="utf-8")
    return {"+".join(sorted((a, b))): float(v)
            for a, b, v in re.findall(r'\{"(\w+)",\s*"(\w+)",\s*([0-9.]+)f\}', text)}


def test_header_keys_are_alphabetical_pairs():
    text = HEADER.read_text(encoding="utf-8")
    for a, b, _ in re.findall(r'\{"(\w+)",\s*"(\w+)",\s*([0-9.]+)f\}', text):
        assert a < b, f"{a}+{b}: fireThreshold looks pairs up in alphabetical order"


@pytest.mark.skipif(not EVAL.is_file(), reason="bench-results/masking_eval.json not present")
def test_header_bars_match_the_corpus_run():
    measured = json.loads(EVAL.read_text(encoding="utf-8")).get("pair_fire_at")
    if not measured:
        pytest.skip("masking_eval.json predates the per-pair bars")
    expected = {"+".join(sorted(k.split("+"))): v for k, v in measured.items()}
    assert _header_bars() == pytest.approx(expected)
