"""Channel-layout contract on the Python side (audit AQ-01, sub-project B).

Mirrors the C++ shared-core/include/khensu/analysis/ChannelLayout.h contract so
the analysis and render halves cannot drift apart.
"""
import pytest

from khensu_services.common.channel_layout import (
    ChannelLayout,
    UnsupportedLayoutError,
    SUPPORTED_LAYOUTS,
)


@pytest.mark.parametrize(
    "ch,name",
    [(1, "Mono"), (2, "Stereo"), (6, "5.1"), (8, "7.1")],
)
def test_supported_layouts_resolve(ch, name):
    layout = ChannelLayout.from_channel_count(ch)
    assert layout.num_channels == ch
    assert layout.name == name


@pytest.mark.parametrize("ch", [0, -1, 3, 4, 5, 7, 9, 64])
def test_unsupported_layouts_raise_with_actionable_message(ch):
    with pytest.raises(UnsupportedLayoutError) as exc:
        ChannelLayout.from_channel_count(ch)
    # The message must name the count and the supported set so a user can act.
    assert str(ch) in str(exc.value)
    assert "Mono" in str(exc.value)


def test_supported_layouts_table_matches_cpp_contract():
    assert SUPPORTED_LAYOUTS == {1: "Mono", 2: "Stereo", 6: "5.1", 8: "7.1"}


def test_51_role_order_is_smpte():
    layout = ChannelLayout.from_channel_count(6)
    assert layout.roles == ("L", "R", "C", "LFE", "Ls", "Rs")
    assert layout.lfe_indices() == (3,)
    assert layout.non_lfe_indices() == (0, 1, 2, 4, 5)
    assert layout.front_pair() == (0, 1)
    assert layout.has_lfe()


def test_71_adds_the_back_pair():
    layout = ChannelLayout.from_channel_count(8)
    assert layout.roles == ("L", "R", "C", "LFE", "Ls", "Rs", "Lb", "Rb")
    assert layout.lfe_indices() == (3,)
    assert layout.non_lfe_indices() == (0, 1, 2, 4, 5, 6, 7)


def test_mono_and_stereo_have_no_lfe():
    for ch in (1, 2):
        layout = ChannelLayout.from_channel_count(ch)
        assert not layout.has_lfe()
        assert layout.lfe_indices() == ()
        assert layout.non_lfe_indices() == tuple(range(ch))


def test_mono_front_pair_folds_to_the_single_channel():
    assert ChannelLayout.from_channel_count(1).front_pair() == (0, 0)
