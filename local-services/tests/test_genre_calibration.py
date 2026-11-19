from khensu_services.evaluation.corpus_schema import CorpusItem, STRATA_AXES, validate_split
from khensu_services.evaluation.genre_calibration import calibration_report


def _item(item_id: str, split: str, artist: str, genre: str, quality: str = "lossless") -> CorpusItem:
    strata = {axis: "balanced" for axis in STRATA_AXES}
    strata.update(genre=genre, source_quality=quality)
    return CorpusItem(item_id, f"licensed/{item_id}.wav", "CC-BY test fixture", artist,
                      f"project-{artist}", split, strata, {"genre": {"subgenre": "unknown"}})


def test_split_rejects_artist_leakage_and_unrepresented_test_genre():
    items = [_item("a", "train", "artist-a", "pop"), _item("b", "test", "artist-a", "pop")]
    result = validate_split(items)
    assert not result["ok"]
    assert any("leaks" in error for error in result["errors"])


def test_calibration_requires_governed_held_out_predictions():
    items = [_item("a", "train", "artist-a", "pop"), _item("b", "test", "artist-b", "pop")]
    report = calibration_report(items, {"b": {"label": "pop", "confidence": 0.9}})
    assert report["ready"]
    assert report["overall"]["accuracy"] == 1.0
    assert "source_quality" in report["by_stratum"]


def test_calibration_rejects_rank_scores_as_confidence_by_omission():
    items = [_item("a", "train", "artist-a", "pop"), _item("b", "test", "artist-b", "pop")]
    report = calibration_report(items, {"b": {"label": "pop", "rank_score": 0.9}})
    assert not report["ready"]
    assert any("confidence" in error for error in report["errors"])
