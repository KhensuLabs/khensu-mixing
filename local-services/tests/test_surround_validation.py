"""Layout-aware render validation and guardrails (audit AQ-01, sub-project B)."""
import os
import tempfile

import numpy as np
import pytest
import soundfile as sf

from khensu_services.copilot.render_guardrails import post_render_guardrail
from khensu_services.copilot.render_validator import validate_render_request

SR = 48000
_N = SR // 2


def _write(path: str, channels: int, seed: int = 5, scale: float = 0.15) -> str:
    rng = np.random.default_rng(seed)
    x = (rng.standard_normal((_N, channels)) * scale) if channels > 1 else \
        (rng.standard_normal(_N) * scale)
    sf.write(path, x, SR, subtype="PCM_24")
    return path


# --- validator -----------------------------------------------------------

@pytest.mark.parametrize("channels,name", [(1, "Mono"), (2, "Stereo"), (6, "5.1"), (8, "7.1")])
def test_supported_layouts_validate(channels, name):
    with tempfile.TemporaryDirectory() as d:
        src = _write(os.path.join(d, "in.wav"), channels)
        r = validate_render_request(src, effects=[{"effect": "Gain", "parameters": {"gain_db": 1.0}}])
        assert r["ok"], r["blocks"]
        assert r["channel_layout"] == name
        assert r["lfe_protected"] is (channels in (6, 8))


@pytest.mark.parametrize("channels", [3, 4, 5, 7])
def test_unsupported_layouts_block_with_an_actionable_message(channels):
    with tempfile.TemporaryDirectory() as d:
        src = _write(os.path.join(d, "in.wav"), channels)
        r = validate_render_request(src, effects=[])
        assert not r["ok"]
        assert any(str(channels) in b for b in r["blocks"])
        assert r["channel_layout"] is None


def test_unclassified_effect_blocks_on_surround():
    """An effect with no declared routing must be refused, not approximated."""
    with tempfile.TemporaryDirectory() as d:
        src = _write(os.path.join(d, "in.wav"), 6)
        r = validate_render_request(src, effects=[{"effect": "NotARealEffect"}])
        assert not r["ok"]
        assert any("NotARealEffect" in b for b in r["blocks"])


def test_unclassified_effect_does_not_block_stereo():
    """Stereo keeps its existing behaviour: unknown effects are skipped by the
    chain, not blocked by the validator."""
    with tempfile.TemporaryDirectory() as d:
        src = _write(os.path.join(d, "in.wav"), 2)
        r = validate_render_request(src, effects=[{"effect": "NotARealEffect"}])
        assert r["ok"], r["blocks"]


# --- guardrails ----------------------------------------------------------

def test_channel_count_change_is_blocked():
    """The generic backstop for the channel-destruction defect class."""
    with tempfile.TemporaryDirectory() as d:
        before = _write(os.path.join(d, "before.wav"), 6)
        after = _write(os.path.join(d, "after.wav"), 2, seed=6)
        r = post_render_guardrail(before, after)
        assert not r["ok"]
        assert any("channel count" in b.lower() for b in r["blocks"])


def test_lfe_altered_beyond_a_uniform_gain_is_blocked():
    with tempfile.TemporaryDirectory() as d:
        before = os.path.join(d, "before.wav")
        after = os.path.join(d, "after.wav")
        rng = np.random.default_rng(11)
        x = rng.standard_normal((_N, 6)) * 0.1
        sf.write(before, x, SR, subtype="PCM_24")
        y = x.copy()
        # Level-dependent change on LFE only: unmistakably effect processing.
        y[:, 3] = np.tanh(y[:, 3] * 8.0) * 0.3
        sf.write(after, y, SR, subtype="PCM_24")

        r = post_render_guardrail(before, after)
        assert any("LFE" in b for b in r["blocks"])


def test_uniform_gain_on_lfe_is_allowed():
    """The shared clip-guard attenuation must not trip the LFE guard."""
    with tempfile.TemporaryDirectory() as d:
        before = os.path.join(d, "before.wav")
        after = os.path.join(d, "after.wav")
        rng = np.random.default_rng(12)
        x = rng.standard_normal((_N, 6)) * 0.1
        sf.write(before, x, SR, subtype="PCM_24")
        sf.write(after, x * 0.92, SR, subtype="PCM_24")

        r = post_render_guardrail(before, after)
        assert not any("LFE" in b for b in r["blocks"]), r["blocks"]


def test_fold_down_excludes_lfe():
    """A loud LFE must not drag the programme-material metrics around."""
    from khensu_services.copilot.render_guardrails import _programme_channels

    y = np.zeros((6, 100))
    y[3, :] = 1.0  # LFE only
    assert _programme_channels(y).shape[0] == 5
    assert np.allclose(_programme_channels(y).mean(axis=0), 0.0)
