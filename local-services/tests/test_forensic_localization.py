import numpy as np

from khensu_services.analysis.artifacts.forensic_tier import CodecResidualForensicDetector

SR = 22050


def _comb_hf(dur, sr):
    """Comb of narrowband tones above 10 kHz -> high HF residual peakiness."""
    t = np.arange(int(dur * sr)) / sr
    y = np.zeros_like(t)
    for f in (10500.0, 10520.0, 10540.0):     # tight comb near 10.5 kHz
        y += 0.25 * np.sin(2 * np.pi * f * t)
    return y.astype(np.float32)


def _clean_tone(dur, sr):
    t = np.arange(int(dur * sr)) / sr
    return (0.3 * np.sin(2 * np.pi * 220 * t)).astype(np.float32)


def test_localized_finding_has_time_range():
    det = CodecResidualForensicDetector()
    # Artifact only in the middle third (2-4 s of a 6 s file).
    y = np.concatenate([_clean_tone(2.0, SR), _comb_hf(2.0, SR), _clean_tone(2.0, SR)])
    findings = det._scan(y.astype(np.float32), SR)
    localized = [f for f in findings if f.time_range_sec is not None]
    assert localized, "expected at least one time-scoped finding"
    lo, hi = localized[0].time_range_sec
    assert 0.0 <= lo < hi <= 6.5      # a non-empty span within the file
    # AID-11: localized findings carry their tell's weight, scoped to their
    # time range (same correlated group, so the whole-file score counts the
    # tell once) - the timeline is no longer flat.
    parents = {f.kind: f.weight for f in findings if f.time_range_sec is None}
    assert all(f.weight == parents.get(f.kind, f.weight) > 0.0 for f in localized)


def test_clean_tone_still_silent():
    det = CodecResidualForensicDetector()
    y = _clean_tone(3.0, SR)
    assert det._scan(y, SR) == []
