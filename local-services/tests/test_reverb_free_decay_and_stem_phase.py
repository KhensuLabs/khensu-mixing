"""Free-decay RT60 (audit M1) and single-resample stem phase (audit M5)."""
from __future__ import annotations

import numpy as np
import pytest
import soundfile as sf

from khensu_services.analysis.performance_analyzer import (
    analyze_stem_phase_correlation, estimate_reverb_tail,
)

SR = 22050


def _bursts(n: int, rt60: float = 0.5, gap: float = 1.5, seed: int = 0) -> np.ndarray:
    rng = np.random.default_rng(seed)
    t = np.arange(int(gap * SR)) / SR
    one = lambda: rng.standard_normal(t.size) * np.exp(-6.91 * t / rt60) * 0.5  # noqa: E731
    return np.concatenate([one() for _ in range(n)]).astype(np.float32)


@pytest.mark.parametrize("n_bursts", [4, 12])
def test_rt60_does_not_scale_with_file_length(tmp_path, n_bursts):
    p = tmp_path / "bursts.wav"
    sf.write(str(p), _bursts(n_bursts), SR)
    res = estimate_reverb_tail(str(p))
    assert res["rt60_sec"] == pytest.approx(0.5, rel=0.25)
    assert res["decay_segments"] >= 1
    assert "free-decay" in res["rt60_note"]


def test_no_free_decay_returns_none_with_note(tmp_path):
    rng = np.random.default_rng(1)
    p = tmp_path / "dense.wav"
    sf.write(str(p), (0.3 * rng.standard_normal(SR * 4)).astype(np.float32), SR)
    res = estimate_reverb_tail(str(p))
    assert res["rt60_sec"] is None
    assert res["decay_segments"] == 0
    assert "No usable free decay" in res["rt60_note"]


def test_stem_phase_resamples_each_stem_once(tmp_path, monkeypatch):
    import librosa

    t = np.arange(SR) / SR
    sig = (0.3 * np.sin(2 * np.pi * 220 * t)).astype(np.float32)
    paths = {}
    for i, rate in enumerate((SR, 2 * SR, 2 * SR, 2 * SR)):
        p = tmp_path / f"s{i}.wav"
        y = sig if rate == SR else librosa.resample(sig, orig_sr=SR, target_sr=rate)
        sf.write(str(p), (-y if i == 3 else y), rate)
        paths[f"s{i}"] = str(p)

    calls = []
    real = librosa.resample

    def counting(*a, **k):
        calls.append(k.get("orig_sr"))
        return real(*a, **k)

    monkeypatch.setattr(librosa, "resample", counting)
    res = analyze_stem_phase_correlation(paths)
    assert len(calls) == 3              # once per off-rate stem, not per pair (was 6)
    assert res["stem_count"] == 4 and len(res["pairs"]) == 6
    inv = [p for p in res["pairs"] if "s3" in (p["left"], p["right"])]
    assert all(p["polarity"] == "inverted" for p in inv)
