# local-services/tests/test_masking.py
"""Tests for stem masking, including the sub-second time x frequency heatmap."""
import numpy as np
import soundfile as sf

from khensu_services.analysis.masking import (
    masking_heatmap, compute_stem_masking, _MASKING_BANDS,
)

SR = 22050


def _tone(freq, dur, sr=SR, amp=0.5):
    t = np.arange(int(dur * sr)) / sr
    return amp * np.sin(2 * np.pi * freq * t)


def _write(tmp_path, name, y, sr=SR):
    p = tmp_path / name
    sf.write(str(p), np.asarray(y, dtype=np.float32), sr)
    return str(p)


def _band_index(freq):
    for i, (lo, hi) in enumerate(_MASKING_BANDS):
        if lo <= freq < hi:
            return i
    raise AssertionError(f"no band for {freq}")


def test_masking_heatmap_localizes_overlap_in_time():
    # Both stems share a 200 Hz tone in the FIRST half; the second stem goes
    # silent in the second half, so the collision should be hot early, cold late.
    dur = 4.0
    a = _tone(200.0, dur)
    b = np.concatenate([_tone(200.0, dur / 2), np.zeros(int((dur / 2) * SR))])
    hm = masking_heatmap(a, SR, b, SR, block_ms=100.0)

    assert hm["n_frames"] > 4
    assert len(hm["bands_hz"]) == len(_MASKING_BANDS)
    assert all(len(row) == len(_MASKING_BANDS) for row in hm["grid"])
    assert all(0.0 <= v <= 1.0 for row in hm["grid"] for v in row)

    bi = _band_index(200.0)
    grid = np.array(hm["grid"])
    first_quarter = grid[: len(grid) // 4, bi].mean()
    last_quarter = grid[-len(grid) // 4:, bi].mean()
    assert first_quarter > last_quarter + 0.2  # collision concentrated early


def test_masking_heatmap_frame_cap():
    # A long signal must stay bounded to max_frames by widening the block.
    a = _tone(200.0, 60.0)
    b = _tone(200.0, 60.0)
    hm = masking_heatmap(a, SR, b, SR, block_ms=50.0, max_frames=200)
    assert hm["n_frames"] <= 200
    assert hm["block_ms"] >= 50.0  # block widened to respect the cap


def test_compute_stem_masking_returns_vocal_heatmap(tmp_path):
    # vocals + other overlap at 200 Hz; drums sits high — worst pair should
    # involve vocals, and a heatmap should be attached.
    vocals = _write(tmp_path, "vocals.wav", _tone(200.0, 3.0))
    other = _write(tmp_path, "other.wav", _tone(210.0, 3.0))
    drums = _write(tmp_path, "drums.wav", _tone(9000.0, 3.0))
    out = compute_stem_masking(
        {"vocals": vocals, "other": other, "drums": drums}
    )
    assert out["heatmap"] is not None
    hm = out["heatmap"]
    assert "vocals" in (hm["stem_a"], hm["stem_b"])
    assert hm["n_frames"] > 0
    assert len(hm["grid"]) == hm["n_frames"]
