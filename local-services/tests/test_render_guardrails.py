# local-services/tests/test_render_guardrails.py
"""Tests for the post-render safety guardrail."""
import numpy as np
import soundfile as sf

from khensu_services.copilot.render_guardrails import post_render_guardrail

SR = 44100


def _write(tmp_path, name, y):
    p = tmp_path / name
    sf.write(str(p), np.asarray(y, dtype=np.float32), SR)
    return str(p)


def _dynamic_stereo(dur=3.0):
    t = np.arange(int(dur * SR)) / SR
    tone = 0.3 * np.sin(2 * np.pi * 200.0 * t)
    # Amplitude envelope so there's real crest factor to squash.
    env = 0.3 + 0.7 * (0.5 + 0.5 * np.sin(2 * np.pi * 2.0 * t))
    mono = tone * env
    return np.stack([mono, mono * 0.98], axis=1)


def test_benign_gain_render_passes(tmp_path):
    src = _dynamic_stereo()
    before = _write(tmp_path, "before.wav", src)
    after = _write(tmp_path, "after.wav", src * 0.5)  # quieter, otherwise identical
    r = post_render_guardrail(before, after)
    assert r["ok"] is True
    assert r["blocks"] == []


def test_hard_limiting_flags_regression(tmp_path):
    src = _dynamic_stereo()
    before = _write(tmp_path, "before.wav", src)
    # Brutal limiting: boost hard then clip to full scale -> crushed crest,
    # clipped samples, hotter true peak.
    smashed = np.clip(src * 6.0, -1.0, 1.0)
    after = _write(tmp_path, "after.wav", smashed)
    r = post_render_guardrail(before, after)
    # Either a hard block (clipping/true-peak) or at least a crest warning must fire.
    assert (r["ok"] is False) or (len(r["warnings"]) > 0)
    assert r["after"]["crest_db"] < r["before"]["crest_db"]


def test_matched_level_reported(tmp_path):
    src = _dynamic_stereo()
    before = _write(tmp_path, "before.wav", src)
    after = _write(tmp_path, "after.wav", src * 0.25)  # -12 dB
    r = post_render_guardrail(before, after)
    # The guardrail should have matched levels (positive gain applied to after).
    assert r["matched_gain_db"] > 3.0


def _noise_source(dur=3.0, seed=7):
    # Band-limited (music-like) noise: clipping it leaves multi-sample flat tops.
    from scipy.signal import butter, sosfilt
    rng = np.random.default_rng(seed)
    x = sosfilt(butter(4, 4000.0, fs=SR, output="sos"), rng.standard_normal(int(dur * SR)))
    x = 0.9 * x / np.max(np.abs(x))
    return np.stack([x, 0.97 * x], axis=1)


def test_clipped_louder_render_blocks_unmatched(tmp_path):
    # AUDIT-005: a 2x-clipped render is louder than the source; matching it
    # down used to hide both the over-ceiling true peak and the flat tops.
    src = _noise_source()
    before = _write(tmp_path, "before.wav", src)
    after = _write(tmp_path, "after.wav", np.clip(src * 2.0, -1.0, 1.0))
    r = post_render_guardrail(before, after)
    assert r["ok"] is False
    assert r["after"]["clip_samples"] > 0
    assert r["after"]["true_peak_dbtp"] > -1.0     # measured as written
    assert any("True peak" in b for b in r["blocks"])
    assert any("clipped" in b for b in r["blocks"])


def test_scaled_down_clipping_still_blocks(tmp_path):
    src = _noise_source()
    before = _write(tmp_path, "before.wav", src)
    after = _write(tmp_path, "after.wav", 0.8 * np.clip(src * 2.0, -1.0, 1.0))
    r = post_render_guardrail(before, after)
    assert r["ok"] is False
    assert r["after"]["clip_samples"] > 0
    assert any("clipped" in b for b in r["blocks"])


def test_clean_sine_crest_is_not_counted_as_clipping():
    from khensu_services.copilot.render_guardrails import count_flat_top_samples
    t = np.arange(SR) / SR
    for f in (40.0, 1000.0):
        assert count_flat_top_samples(np.sin(2 * np.pi * f * t)[np.newaxis, :]) == 0
