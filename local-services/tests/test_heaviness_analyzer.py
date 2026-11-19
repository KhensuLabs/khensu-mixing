from __future__ import annotations
import numpy as np
from khensu_services.analysis.heaviness_analyzer import score_heaviness

SR = 22050


def test_score_heaviness_silence_has_zero_distortion():
    y = np.zeros(SR * 3, dtype=np.float32)
    result = score_heaviness(y, SR)
    assert result["heaviness_distortion"] == 0.0
    for v in result.values():
        assert 0.0 <= v <= 1.0


def test_score_heaviness_white_noise_higher_distortion():
    rng = np.random.default_rng(42)
    y = (rng.standard_normal(SR * 3) * 0.3).astype(np.float32)
    result = score_heaviness(y, SR)
    assert result["heaviness_distortion"] > 0.3
    for v in result.values():
        assert 0.0 <= v <= 1.0


def test_score_heaviness_all_keys_present():
    y = np.zeros(SR * 2, dtype=np.float32)
    result = score_heaviness(y, SR)
    assert "heaviness_distortion" in result
    assert "heaviness_weight"     in result
    assert "heaviness_clarity"    in result
    assert "heaviness_extremity"  in result
