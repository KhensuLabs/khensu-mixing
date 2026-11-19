"""The render guardrail's deadline and its source-side cache (2026-11-18 soak).

The check measured the whole source again for every render and had a fixed
60 s deadline: 50 s on a 4-minute track idle, so fixes under load timed out,
and every fix of a long track would have.
"""
from __future__ import annotations

import numpy as np
import soundfile as sf

from khensu_services.copilot import render_guardrails as rg


def _wav(path, seconds: float, sr: int = 22050) -> str:
    rng = np.random.default_rng(1)
    y = 0.1 * rng.standard_normal((int(seconds * sr), 2))
    sf.write(str(path), y, sr)
    return str(path)


def test_the_deadline_grows_with_the_track(tmp_path):
    short = _wav(tmp_path / "short.wav", 10)
    long = _wav(tmp_path / "long.wav", 200)
    assert rg.guardrail_timeout_s(short) == 60.0                      # the floor
    assert rg.guardrail_timeout_s(long) == 0.75 * 200                  # scales
    assert rg.guardrail_timeout_s(str(tmp_path / "missing.wav")) == 60.0
    assert rg.guardrail_timeout_s(short, floor_s=5.0) == 7.5


def test_the_source_side_is_measured_once_per_file(tmp_path, monkeypatch):
    src = _wav(tmp_path / "src.wav", 5)
    out1 = _wav(tmp_path / "out1.wav", 5)
    out2 = _wav(tmp_path / "out2.wav", 5)
    rg._SOURCE_SIDE.clear()
    calls: list[str] = []
    real = rg._perceptual_metrics

    def counting(y, sr):
        calls.append("p")
        return real(y, sr)

    monkeypatch.setattr(rg, "_perceptual_metrics", counting)
    first = rg.post_render_guardrail(src, out1)
    n_first = len(calls)                       # source + render
    second = rg.post_render_guardrail(src, out2)
    assert n_first == 2
    assert len(calls) - n_first == 1           # the render only: the source came from the cache
    assert first["before"] == second["before"]
