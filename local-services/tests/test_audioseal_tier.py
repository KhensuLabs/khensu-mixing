"""AudioSeal watermark detector: gated-off contract (no test coverage existed
before - found during a 2026-07-17 survey of ML-model coverage across the
analyzer layer). Does not require the audioseal package or its downloaded
weights: the detector must degrade to "no findings, never raises" whenever
the gate is closed, which is the default (KHENSU_ENABLE_WATERMARK_MODELS
unset and no pre-cached weights) in CI and on a fresh install.

The bottom test is a real, opt-in false-positive check against
corpus_data/mtg_jamendo_real/ (see scripts/verify_audioseal_false_positive_rate.py
and corpus_data/README.md) - skipped automatically unless that directory and
the audioseal package are both present, since neither exists in CI.
"""
import glob
import os

import numpy as np
import pytest

from khensu_services.analysis.artifacts.audioseal_tier import AudioSealWatermarkDetector
from khensu_services.analysis.artifacts.base import AudioContext

_JAMENDO_DIR = os.path.join(os.path.dirname(__file__), "..", "..", "corpus_data",
                           "mtg_jamendo_real", "extracted")


def _ctx(seconds=2.0, sr=22050):
    t = np.arange(int(sr * seconds)) / sr
    y = (0.2 * np.sin(2 * np.pi * 440 * t)).astype(np.float32)
    return AudioContext("x.wav", y, sr)


def test_gated_off_by_default_returns_no_findings(monkeypatch):
    monkeypatch.delenv("KHENSU_ENABLE_WATERMARK_MODELS", raising=False)
    assert AudioSealWatermarkDetector().scan(_ctx()) == []


def test_never_raises_when_gate_closed(monkeypatch):
    monkeypatch.delenv("KHENSU_ENABLE_WATERMARK_MODELS", raising=False)
    # Multiple calls, including a too-short clip, must never raise regardless
    # of whether the audioseal package is importable in this environment.
    AudioSealWatermarkDetector().scan(_ctx(seconds=0.1))
    AudioSealWatermarkDetector().scan(_ctx(seconds=2.0))


def _audioseal_installed() -> bool:
    import importlib.util
    return importlib.util.find_spec("audioseal") is not None


@pytest.mark.skipif(not glob.glob(os.path.join(_JAMENDO_DIR, "*.opus")),
                    reason="corpus_data/mtg_jamendo_real/extracted not present (local-only dataset)")
@pytest.mark.skipif(not _audioseal_installed(), reason="audioseal package not installed")
def test_false_positive_rate_on_real_music_is_low(monkeypatch):
    """2026-07-17: verified 0/300 false positives at the production 0.5
    threshold against a real, diverse MTG-Jamendo sample (see
    scripts/verify_audioseal_false_positive_rate.py for the full run). This
    is a small (10-track), fast regression version of that check - it must
    stay at zero; if the model or threshold ever changes and this starts
    firing, that's the signal to re-run the full 300-track verification."""
    monkeypatch.setenv("KHENSU_ENABLE_WATERMARK_MODELS", "1")
    import librosa

    files = sorted(glob.glob(os.path.join(_JAMENDO_DIR, "*.opus")))[:10]
    detector = AudioSealWatermarkDetector()
    flagged = 0
    for f in files:
        y, sr = librosa.load(f, sr=22050, mono=True, duration=10.0)
        if len(y) < sr:
            continue
        findings = detector.scan(AudioContext(f, y.astype(np.float32), sr))
        flagged += len(findings) > 0
    assert flagged == 0, f"{flagged}/{len(files)} real tracks incorrectly flagged as watermarked"
