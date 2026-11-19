# local-services/tests/test_corpus_schema.py
"""Tests for the evaluation-corpus split validator (audit P2.1 scaffold)."""
from khensu_services.evaluation.corpus_schema import (
    CorpusItem, validate_split, STRATA_AXES,
)


def _item(item_id, artist, project, split):
    return CorpusItem(
        item_id=item_id, audio_path=f"/x/{item_id}.wav",
        provenance="licensed:internal-demo", artist_id=artist, project_id=project,
        split=split, strata={ax: "x" for ax in STRATA_AXES},
    )


def test_clean_split_validates():
    items = [_item("a1", "artistA", "projA", "train"),
             _item("b1", "artistB", "projB", "val"),
             _item("c1", "artistC", "projC", "test")]
    r = validate_split(items)
    assert r["ok"] is True
    assert r["errors"] == []


def test_artist_leakage_fails_closed():
    # Same artist in train AND test -> leakage -> must fail.
    items = [_item("a1", "artistA", "projA", "train"),
             _item("a2", "artistA", "projA", "test")]
    r = validate_split(items)
    assert r["ok"] is False
    assert any("leaks across splits" in e for e in r["errors"])


def test_unknown_provenance_and_missing_strata_fail():
    it = CorpusItem(item_id="x", audio_path="/x.wav", provenance="unknown",
                    artist_id="a", project_id="p", split="train",
                    strata={"genre": "trap"})  # missing most axes
    r = validate_split([it])
    assert r["ok"] is False
    assert any("provenance is required" in e for e in r["errors"])
    assert any("missing strata" in e for e in r["errors"])
