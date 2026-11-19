"""Unit tests for sonics_corpus.py's Clip construction that don't need the
external Model Weights/ or corpus_data/ audio (those are large, local-only
data dirs not present in CI - see corpus_data/README.md). Covers only the
pure filename-parsing logic, in particular the source-tagging fix: Clip
previously had no `source` field at all, so source_balance.py's
balance_by_source()/per_source_metrics() silently lumped every clip under
"unknown" regardless of whether it came from SONICS, MUSDB18, or (as of
2026-07-17) MTG-Jamendo.
"""
from khensu_services.analysis.learned.sonics_corpus import _fake_clips


def test_fake_clips_tags_source_from_generator_in_filename(tmp_path):
    fake_dir = tmp_path / "fake_songs"
    fake_dir.mkdir()
    (fake_dir / "fake_00001_suno_0.mp3").write_bytes(b"")
    (fake_dir / "fake_00001_suno_1.mp3").write_bytes(b"")
    (fake_dir / "fake_00002_udio_0.mp3").write_bytes(b"")

    clips = _fake_clips(str(tmp_path), limit=10)

    assert len(clips) == 3
    assert all(c.label == 1 for c in clips)
    sources = {c.source for c in clips}
    assert sources == {"sonics_suno", "sonics_udio"}


def test_fake_clips_groups_takes_of_the_same_song_together(tmp_path):
    fake_dir = tmp_path / "fake_songs"
    fake_dir.mkdir()
    (fake_dir / "fake_00001_suno_0.mp3").write_bytes(b"")
    (fake_dir / "fake_00001_suno_1.mp3").write_bytes(b"")

    clips = _fake_clips(str(tmp_path), limit=10)

    groups = {c.group for c in clips}
    assert groups == {"fake_00001"}, "both takes of the same song must share one leakage-safe group"
