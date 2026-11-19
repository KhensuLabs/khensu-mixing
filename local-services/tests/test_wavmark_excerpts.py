"""WavMark detector: bounded excerpt scan instead of a whole-file search.

decode_watermark over a whole track cost ~7.5x the track's duration on CPU,
so the desktop's AI deep scan (5 min budget) never finished on a real song.
The detector now decodes three short excerpts; these tests pin the excerpt
maths and, where the model is cached, that a watermark is still found.
"""
from __future__ import annotations

import numpy as np
import pytest

from khensu_services.analysis.artifacts.wavmark_tier import excerpt_bounds

SR = 16000


def test_short_signals_are_scanned_whole():
    assert excerpt_bounds(5 * SR) == [(0, 5 * SR)]
    assert excerpt_bounds(9 * SR) == [(0, 9 * SR)]


def test_long_signals_get_three_fixed_excerpts_inside_the_signal():
    n = 180 * SR  # a 3-minute song
    bounds = excerpt_bounds(n)
    assert len(bounds) == 3
    for start, end in bounds:
        assert 0 <= start < end <= n
        assert end - start == 3 * SR
    # Spread through the track, in order.
    starts = [s for s, _ in bounds]
    assert starts == sorted(starts)
    assert starts[0] < n * 0.3 and starts[-1] > n * 0.7


def test_excerpt_count_does_not_grow_with_length():
    # The point: cost is bounded no matter how long the track is.
    assert len(excerpt_bounds(10 * 60 * SR)) == len(excerpt_bounds(60 * SR)) == 3


def test_excerpts_near_the_edges_are_clamped():
    n = 10 * SR
    for start, end in excerpt_bounds(n):
        assert start >= 0 and end <= n


def test_real_watermark_found_from_excerpts_and_clean_audio_is_not():
    wavmark = pytest.importorskip("wavmark")
    from khensu_services.analysis.artifacts import wavmark_tier
    from khensu_services.analysis.artifacts.base import AudioContext

    if wavmark_tier.unavailable_reason() is not None:
        pytest.skip(f"WavMark model unavailable: {wavmark_tier.unavailable_reason()}")
    model = wavmark_tier._model()
    if model is None:
        pytest.skip("WavMark model failed to load here")

    rng = np.random.default_rng(3)
    t = np.arange(30 * SR) / SR
    clean = (0.2 * np.sin(2 * np.pi * 220 * t) + 0.02 * rng.standard_normal(t.size)).astype(np.float32)
    payload = rng.integers(0, 2, size=16)
    marked, _ = wavmark.encode_watermark(model, clean, payload, show_progress=False)

    det = wavmark_tier.WavMarkWatermarkDetector()
    found = det.scan(AudioContext(file_path="marked.wav", mono=np.asarray(marked, dtype=np.float32), sr=SR))
    assert found and found[0].kind == "wavmark_watermark"
    assert det.scan(AudioContext(file_path="clean.wav", mono=clean, sr=SR)) == []
