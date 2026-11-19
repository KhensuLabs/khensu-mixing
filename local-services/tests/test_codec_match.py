"""Source/codec confound control for the learned AI detector (audit AI #1).

SONICS positives are MP3; the real negatives (MUSDB18 .mp4/AAC, Jamendo .opus)
are not, so a detector can shortcut on "MP3 encoder artifacts = AI". The fix is
to MP3-round-trip the real negatives through the same codec path. These tests
cover (a) build_corpus tagging only the negatives, and (b) the round-trip
actually altering audio while staying finite/same-length. The round-trip test
skips cleanly where ffmpeg+libmp3lame isn't installed (e.g. minimal CI), since
the corpus/retrain itself is opt-in and its data is git-ignored.
"""
from __future__ import annotations

import shutil
import subprocess

import numpy as np
import pytest

from khensu_services.analysis.learned.sonics_corpus import Clip
from khensu_services.analysis.learned.train_sonics import SR, _mp3_roundtrip


def _has_lame() -> bool:
    if shutil.which("ffmpeg") is None:
        return False
    try:
        out = subprocess.run(["ffmpeg", "-hide_banner", "-encoders"],
                             capture_output=True, text=True, timeout=15)
    except Exception:
        return False
    return "libmp3lame" in out.stdout


# --- corpus tagging ---------------------------------------------------------

def test_codec_match_flag_defaults_off():
    assert Clip(path="x", label=0, group="g").codec_match_mp3_k == 0


def test_build_corpus_tags_only_negatives(monkeypatch):
    import khensu_services.analysis.learned.sonics_corpus as sc

    fake = [Clip(path="f.mp3", label=1, group="fake_1", source="sonics_suno")]
    real = [Clip(path="r.opus", label=0, group="jamendo_1", source="jamendo")]
    monkeypatch.setattr(sc, "_fake_clips", lambda *a, **k: list(fake))
    monkeypatch.setattr(sc, "_musdb_clips", lambda *a, **k: list(real))
    monkeypatch.setattr(sc, "_jamendo_clips", lambda *a, **k: [])
    monkeypatch.setattr(sc, "_probe_bitrate_k", lambda p: 128)

    train, val = sc.build_corpus(jamendo_root=None, codec_match=True, val_frac=0.5)
    clips = train + val
    for c in clips:
        if c.label == 1:
            assert c.codec_match_mp3_k == 0      # positives already MP3
        else:
            assert c.codec_match_mp3_k == 128     # negatives get matched


def test_build_corpus_no_tagging_when_disabled(monkeypatch):
    import khensu_services.analysis.learned.sonics_corpus as sc

    monkeypatch.setattr(sc, "_fake_clips", lambda *a, **k: [
        Clip(path="f.mp3", label=1, group="fake_1", source="sonics_suno")])
    monkeypatch.setattr(sc, "_musdb_clips", lambda *a, **k: [
        Clip(path="r.opus", label=0, group="jamendo_1", source="jamendo")])
    monkeypatch.setattr(sc, "_jamendo_clips", lambda *a, **k: [])

    train, val = sc.build_corpus(jamendo_root=None, codec_match=False, val_frac=0.5)
    assert all(c.codec_match_mp3_k == 0 for c in train + val)


# --- round-trip mechanism ---------------------------------------------------

@pytest.mark.skipif(not _has_lame(), reason="ffmpeg+libmp3lame not available")
def test_mp3_roundtrip_alters_audio_but_preserves_shape():
    t = np.arange(SR * 2) / SR
    # A tone plus wideband content so the MP3 psychoacoustic model has something
    # to discard - a pure sine can survive nearly intact.
    y = (0.3 * np.sin(2 * np.pi * 440 * t)
         + 0.05 * np.random.default_rng(0).standard_normal(t.size)).astype(np.float32)

    rt = _mp3_roundtrip(y, SR, 96)
    assert rt is not None
    assert np.isfinite(rt).all()
    # Same length (within an MP3 encoder-delay frame or so) and actually changed.
    assert abs(rt.size - y.size) < 4096
    n = min(rt.size, y.size)
    assert not np.allclose(rt[:n], y[:n], atol=1e-4)
