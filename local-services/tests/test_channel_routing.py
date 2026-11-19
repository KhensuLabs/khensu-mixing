"""Channel-role routing for the render path (audit AQ-01, sub-project B).

The shape assertion in route_effect is the load-bearing safety mechanism: it
catches, generically, the class of bug where an effect rebuilds its output with
a hardcoded channel stack and silently discards channels mid-render.
"""
import numpy as np
import pytest

from khensu_services.common.channel_layout import ChannelLayout
from khensu_services.copilot.channel_routing import (
    ChannelContractError,
    Routing,
    route_effect,
)


def _buf(ch: int, n: int = 256) -> np.ndarray:
    """Distinct constant per channel, so channel identity is checkable."""
    return np.stack([np.full(n, float(c + 1)) for c in range(ch)], axis=0)


def test_per_channel_leaves_lfe_untouched_on_51():
    layout = ChannelLayout.from_channel_count(6)
    audio = _buf(6)

    # A gain effect that would wreck the LFE if it were routed there.
    out = route_effect(lambda x: x * 10.0, audio, layout, Routing.PER_CHANNEL)

    assert out.shape == audio.shape
    for c in layout.non_lfe_indices():
        assert np.allclose(out[c], audio[c] * 10.0)
    # LFE (index 3) is bit-identical.
    assert np.array_equal(out[3], audio[3])


def test_front_pair_only_touches_channels_0_and_1_on_51():
    layout = ChannelLayout.from_channel_count(6)
    audio = _buf(6)

    out = route_effect(lambda x: x * -1.0, audio, layout, Routing.FRONT_PAIR_ONLY)

    assert out.shape == audio.shape
    assert np.allclose(out[0], audio[0] * -1.0)
    assert np.allclose(out[1], audio[1] * -1.0)
    for c in (2, 3, 4, 5):
        assert np.array_equal(out[c], audio[c]), f"channel {c} should pass through"


def test_front_pair_receives_exactly_two_channels():
    """The existing 2-channel M/S implementations rely on this."""
    layout = ChannelLayout.from_channel_count(6)
    seen = {}

    def spy(x):
        seen["shape"] = x.shape
        return x

    route_effect(spy, _buf(6), layout, Routing.FRONT_PAIR_ONLY)
    assert seen["shape"][0] == 2


def test_per_channel_receives_only_non_lfe_channels():
    layout = ChannelLayout.from_channel_count(6)
    seen = {}

    def spy(x):
        seen["shape"] = x.shape
        return x

    route_effect(spy, _buf(6), layout, Routing.PER_CHANNEL)
    assert seen["shape"][0] == 5  # 6 channels minus LFE


def test_wrong_channel_count_fails_closed():
    """The bass-mono / mid-side defect class: an effect that rebuilds its
    output with a hardcoded 2-channel stack must fail loudly, never truncate."""
    layout = ChannelLayout.from_channel_count(6)

    def destroys_channels(x):
        # Exactly what _apply_bass_mono_maker used to do to a 6-channel buffer.
        return np.stack([x[0], x[1]], axis=0)

    with pytest.raises(ChannelContractError) as exc:
        route_effect(destroys_channels, _buf(6), layout, Routing.PER_CHANNEL)
    assert "channel" in str(exc.value).lower()


def test_wrong_sample_count_fails_closed():
    layout = ChannelLayout.from_channel_count(6)
    with pytest.raises(ChannelContractError):
        route_effect(lambda x: x[:, :10], _buf(6), layout, Routing.PER_CHANNEL)


@pytest.mark.parametrize("ch", [1, 2])
@pytest.mark.parametrize("routing", [Routing.PER_CHANNEL, Routing.FRONT_PAIR_ONLY])
def test_mono_and_stereo_are_pass_through(ch, routing):
    """Regression guard: for mono/stereo the routed subset is the whole buffer,
    so today's render numbers cannot move."""
    layout = ChannelLayout.from_channel_count(ch)
    audio = _buf(ch)
    seen = {}

    def spy(x):
        seen["shape"] = x.shape
        return x * 2.0

    out = route_effect(spy, audio, layout, routing)
    assert seen["shape"] == audio.shape          # the effect saw everything
    assert np.allclose(out, audio * 2.0)         # and nothing was masked off


def test_mono_front_pair_does_not_duplicate_the_channel():
    layout = ChannelLayout.from_channel_count(1)
    audio = _buf(1)
    out = route_effect(lambda x: x * 3.0, audio, layout, Routing.FRONT_PAIR_ONLY)
    assert out.shape == audio.shape
    assert np.allclose(out, audio * 3.0)
