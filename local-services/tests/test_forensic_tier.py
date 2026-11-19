# local-services/tests/test_forensic_tier.py
"""Tests for the codec-residual forensic detector + AudioSeal registration."""
import numpy as np

from khensu_services.analysis.artifacts.forensic_tier import CodecResidualForensicDetector


SR = 44100


def _t(dur):
    return np.arange(int(dur * SR)) / SR


def test_clean_tone_does_not_fire():
    # A clean tone + gentle noise must not trip the conservative thresholds.
    t = _t(3.0)
    y = 0.3 * np.sin(2 * np.pi * 440 * t) + 0.01 * np.random.default_rng(0).standard_normal(len(t))
    findings = CodecResidualForensicDetector()._scan(y.astype(np.float32), SR)
    assert findings == []


def test_combed_hf_fires_rvq_ghosting():
    # Inject strong comb-like narrowband peaks above 10 kHz (RVQ-ghosting-like).
    t = _t(3.0)
    y = 0.3 * np.sin(2 * np.pi * 220 * t)
    for f in (11000, 13000, 15000, 17000):        # sparse HF tones -> peaky residual
        y = y + 0.15 * np.sin(2 * np.pi * f * t)
    findings = CodecResidualForensicDetector()._scan(y.astype(np.float32), SR)
    kinds = {f.kind for f in findings}
    assert "rvq_ghosting" in kinds
    # Every finding carries measured evidence (never a bare verdict).
    assert all(f.evidence for f in findings)


def test_registered_in_engine():
    from khensu_services.analysis.artifacts import engine
    ids = {d.id for d in engine._DETECTORS}
    assert "codec_residual_forensic" in ids
    assert "audioseal_watermark" in ids
