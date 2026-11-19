"""Learned watermark tiers stay off the fast intake path (pipeline 2026-07-23).

The learned watermark models (AudioSeal ~10s, WavMark's blind decode = minutes)
must never run on the synchronous intake artifact scan — only when a caller
explicitly asks for the deep pass. These tests pin the registry split and the
include_learned wiring without loading the real (slow) models.
"""
from __future__ import annotations

import numpy as np
import soundfile as sf

from khensu_services.analysis.artifacts import analyze_artifacts
from khensu_services.analysis.artifacts import engine

_LEARNED_WATERMARK_IDS = {"audioseal_watermark", "wavmark_watermark"}


def _tiny(path, sr=22050, secs=1.0):
    t = np.arange(int(sr * secs)) / sr
    sf.write(str(path), (0.2 * np.sin(2 * np.pi * 220 * t)).astype(np.float32), sr)
    return str(path)


def test_learned_watermark_detectors_are_not_in_the_fast_group():
    fast_ids = {getattr(d, "id", "") for d in engine._FAST_DETECTORS}
    assert _LEARNED_WATERMARK_IDS.isdisjoint(fast_ids)
    # ...but they ARE in the learned group.
    learned_ids = {getattr(d, "id", "") for d in engine._LEARNED_DETECTORS}
    assert _LEARNED_WATERMARK_IDS <= learned_ids
    # The full registry alias is the union.
    assert {getattr(d, "id", "") for d in engine._DETECTORS} == fast_ids | learned_ids


def test_default_scan_runs_fast_tiers_and_skips_learned_watermark(tmp_path):
    out = analyze_artifacts(_tiny(tmp_path / "a.wav"))
    run = set(out["detectors_run"])
    assert _LEARNED_WATERMARK_IDS.isdisjoint(run)     # fast path: never run
    assert "spectral_ceiling" in run                   # but the fast tiers did


def test_include_learned_runs_the_learned_group(tmp_path, monkeypatch):
    # Swap the learned group for a cheap stub so we prove the wiring without
    # loading the real (slow) watermark models.
    class _Stub:
        id = "stub_learned"
        def scan(self, ctx):
            return []
    monkeypatch.setattr(engine, "_LEARNED_DETECTORS", [_Stub()])

    out = analyze_artifacts(_tiny(tmp_path / "a.wav"), include_learned=True)
    run = out["detectors_run"]
    assert "stub_learned" in run                        # deep pass ran the group
    assert "spectral_ceiling" in run                     # and still the fast tiers


def test_default_scan_ignores_the_learned_group(tmp_path, monkeypatch):
    class _Boom:
        id = "should_not_run"
        def scan(self, ctx):
            raise AssertionError("learned detector ran on the fast path")
    monkeypatch.setattr(engine, "_LEARNED_DETECTORS", [_Boom()])
    # Default (include_learned=False) must not touch the learned group at all.
    out = analyze_artifacts(_tiny(tmp_path / "a.wav"))
    assert "should_not_run" not in out["detectors_run"]


# --- BE-07: shared magnitude-STFT memo on AudioContext ----------------------

def test_abs_stft_memoizes_and_matches_librosa():
    import librosa
    from khensu_services.analysis.artifacts.base import AudioContext

    sr = 22050
    y = (0.3 * np.sin(2 * np.pi * 300 * np.arange(sr * 2) / sr)).astype(np.float32)
    ctx = AudioContext(file_path="x.wav", mono=y, sr=sr)

    a = ctx.abs_stft(2048, 512)
    b = ctx.abs_stft(2048, 512)
    assert a is b                                   # second call is cached
    # Bit-identical to a direct compute (golden-safety of the share).
    assert np.array_equal(a, np.abs(librosa.stft(y, n_fft=2048, hop_length=512)))
    # A different (n_fft, hop) is a separate entry, not the same array.
    assert ctx.abs_stft(4096, 2048) is not a
