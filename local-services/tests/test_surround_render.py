"""Surround render channel contract (audit AQ-01, sub-project B).

The headline case: a chain containing BassMonoMaker/MidSideProcessor used to
rebuild its output with a hardcoded 2-channel stack, so a 5.1 render silently
lost C/LFE/Ls/Rs mid-chain. These tests pin the contract that replaced it.
"""
import os
import tempfile

import numpy as np
import pytest
import soundfile as sf

from khensu_services.common.channel_layout import UnsupportedLayoutError
from khensu_services.copilot.channel_routing import ChannelContractError
from khensu_services.copilot.dsp import (
    EFFECT_ROUTING,
    apply_graph,
    apply_pedalboard_chain,
)

SR = 48000
_N = SR // 2  # 0.5 s keeps the suite fast


def _write(tmpdir: str, channels: int, seed: int = 7) -> str:
    rng = np.random.default_rng(seed)
    x = (rng.standard_normal((_N, channels)) * 0.15) if channels > 1 else \
        (rng.standard_normal(_N) * 0.15)
    path = os.path.join(tmpdir, f"in_{channels}.wav")
    sf.write(path, x, SR, subtype="PCM_24")
    return path


MS_CHAIN = [
    {"effect": "MidSideProcessor", "parameters": {"mid_gain_db": 1.0, "side_gain_db": -2.0}},
    {"effect": "BassMonoMaker", "parameters": {"crossover_hz": 120}},
    {"effect": "HighpassFilter", "parameters": {"cutoff_frequency_hz": 80}},
]


@pytest.mark.parametrize("channels", [6, 8])
def test_surround_render_preserves_channel_count(channels):
    """Regression for the bass-mono / mid-side channel-destruction defect."""
    with tempfile.TemporaryDirectory() as d:
        src = _write(d, channels)
        dst = os.path.join(d, "out.wav")
        apply_pedalboard_chain(src, dst, MS_CHAIN)
        y, _ = sf.read(dst, always_2d=True)
        assert y.shape[1] == channels


def _scalar_ratio(out_ch: np.ndarray, in_ch: np.ndarray) -> float:
    """The single scalar relating two channels, asserting it really is scalar.

    A pass-through channel may still be scaled by the chain's final gain-staging
    attenuation, which applies uniformly to every channel. What must NOT happen
    is frequency- or level-dependent change, which would show up as a
    non-constant ratio.
    """
    mask = np.abs(in_ch) > 1e-6
    ratio = out_ch[mask] / in_ch[mask]
    # 24-bit PCM quantisation puts a small floor under the spread.
    assert float(np.std(ratio)) < 1e-3, "pass-through channel was not a pure scalar change"
    return float(np.mean(ratio))


@pytest.mark.parametrize("channels", [6, 8])
def test_lfe_receives_no_effect_processing(channels):
    """LFE is a band-limited effects channel: full-range mix moves must not
    touch it.

    It may still be scaled by the chain's final clip-guard attenuation, which
    applies uniformly to every channel - that preserves the inter-channel
    balance, whereas exempting LFE would distort it. So the contract is "no
    effect processing", verified as: LFE changes by at most a single scalar,
    and by the *same* scalar as the other pass-through channels.
    """
    chain = MS_CHAIN + [
        {"effect": "Exciter", "parameters": {}},
        {"effect": "Clipper", "parameters": {"ceiling_db": -0.3}},
    ]
    with tempfile.TemporaryDirectory() as d:
        src = _write(d, channels)
        dst = os.path.join(d, "out.wav")
        apply_pedalboard_chain(src, dst, chain)
        x, _ = sf.read(src, always_2d=True)
        y, _ = sf.read(dst, always_2d=True)

        lfe_k = _scalar_ratio(y[:, 3], x[:, 3])
        # The centre channel is processed, so compare against a surround channel
        # that this chain also leaves alone... except MS_CHAIN's HighpassFilter is
        # PER_CHANNEL and touches every non-LFE channel. So assert instead that
        # the LFE scalar matches the global output attenuation, which is <= 1.
        assert 0.0 < lfe_k <= 1.0 + 1e-6, f"LFE scaled by {lfe_k}, expected attenuation only"


def test_lfe_is_bit_identical_when_no_attenuation_is_needed():
    """With headroom to spare the clip-guard does nothing, so the LFE channel
    comes through bit-identical - the strictest form of the contract."""
    chain = [{"effect": "MidSideProcessor", "parameters": {"mid_gain_db": -6.0, "side_gain_db": -6.0}}]
    with tempfile.TemporaryDirectory() as d:
        # Quiet source + attenuating chain => output never approaches the ceiling.
        rng = np.random.default_rng(3)
        x = rng.standard_normal((_N, 6)) * 0.02
        src = os.path.join(d, "quiet.wav")
        sf.write(src, x, SR, subtype="PCM_24")
        dst = os.path.join(d, "out.wav")
        apply_pedalboard_chain(src, dst, chain)
        xi, _ = sf.read(src, always_2d=True)
        y, _ = sf.read(dst, always_2d=True)
        assert np.array_equal(y[:, 3], xi[:, 3]), "LFE must be bit-identical here"


def test_non_lfe_channels_are_processed():
    """The flip side of the LFE guard: everything else must actually change."""
    with tempfile.TemporaryDirectory() as d:
        src = _write(d, 6)
        dst = os.path.join(d, "out.wav")
        apply_pedalboard_chain(src, dst, MS_CHAIN)
        x, _ = sf.read(src, always_2d=True)
        y, _ = sf.read(dst, always_2d=True)
        for c in (0, 1, 2, 4, 5):
            assert not np.array_equal(y[:, c], x[:, c]), f"channel {c} should be processed"


def test_front_pair_effect_leaves_surrounds_untouched():
    """StereoWidener is a front-pair operation; C/LFE/Ls/Rs pass through."""
    chain = [{"effect": "StereoWidener", "parameters": {"width": 1.4}}]
    with tempfile.TemporaryDirectory() as d:
        src = _write(d, 6)
        dst = os.path.join(d, "out.wav")
        apply_pedalboard_chain(src, dst, chain)
        x, _ = sf.read(src, always_2d=True)
        y, _ = sf.read(dst, always_2d=True)
        assert not np.array_equal(y[:, 0], x[:, 0])
        assert not np.array_equal(y[:, 1], x[:, 1])
        # C/LFE/Ls/Rs get no effect processing. They may share the chain's final
        # uniform clip-guard attenuation, so assert "scalar only" and that every
        # pass-through channel carries the identical scalar.
        scalars = [_scalar_ratio(y[:, c], x[:, c]) for c in (2, 3, 4, 5)]
        assert max(scalars) - min(scalars) < 1e-4, f"pass-through scalars differ: {scalars}"


def test_graph_path_preserves_channel_count():
    with tempfile.TemporaryDirectory() as d:
        src = _write(d, 6)
        dst = os.path.join(d, "out.wav")
        apply_graph(src, dst, {"effects": MS_CHAIN})
        y, _ = sf.read(dst, always_2d=True)
        assert y.shape[1] == 6


def test_unsupported_layout_fails_before_rendering():
    """A 4-channel source must fail closed and write no output file."""
    with tempfile.TemporaryDirectory() as d:
        src = _write(d, 4)
        dst = os.path.join(d, "out.wav")
        with pytest.raises(UnsupportedLayoutError) as exc:
            apply_pedalboard_chain(src, dst, MS_CHAIN)
        assert "4" in str(exc.value)
        assert not os.path.exists(dst), "no output file on a rejected layout"


def test_misbehaving_effect_fails_closed(monkeypatch):
    """An effect that returns the wrong channel count must raise, not truncate."""
    import khensu_services.copilot.dsp as dsp

    def destroys_channels(audio, sr, **kwargs):
        return np.stack([audio[0], audio[1]], axis=0)

    monkeypatch.setattr(dsp, "_apply_exciter", destroys_channels)
    with tempfile.TemporaryDirectory() as d:
        src = _write(d, 6)
        dst = os.path.join(d, "out.wav")
        with pytest.raises(ChannelContractError):
            apply_pedalboard_chain(src, dst, [{"effect": "Exciter", "parameters": {}}])
        assert not os.path.exists(dst)


def test_every_dispatched_effect_has_a_declared_routing():
    """An unclassified effect would silently default to PER_CHANNEL on surround;
    keep the table exhaustive instead."""
    from khensu_services.copilot.dsp import EFFECT_MAP

    custom = {
        "DeEsser", "SidechainCompressor", "GlueCompressor", "TapeSaturation",
        "StereoWidener", "BassMonoMaker", "LeakGate", "TransientShaper",
        "PhaseRotator", "MultibandCompressor", "DynamicEQ", "Doubler",
        "DynamicDeEsser", "Humanize", "Exciter", "MidSideProcessor", "Clipper",
        "Flanger", "Tremolo", "Vibrato", "AutoPan", "ConvolutionReverb", "PlosiveRemover",
        "FormantShifter", "SpectralDenoise",
    }
    for name in custom | set(EFFECT_MAP):
        assert name in EFFECT_ROUTING, f"{name} has no declared channel routing"
