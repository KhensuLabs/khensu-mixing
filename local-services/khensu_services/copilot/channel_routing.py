# local-services/khensu_services/copilot/channel_routing.py
"""Channel-role routing for render effects (audit AQ-01, sub-project B).

Effects fall into two routing classes:

* ``PER_CHANNEL`` - broadband processing (EQ, dynamics, saturation). Applied to
  every non-LFE channel. LFE is a band-limited effects channel, not programme
  material, so full-range mix decisions must never touch it; it passes through
  bit-identical.
* ``ALL_CHANNELS`` - level-only stages (Gain, the true-peak Limiter). A level
  move or a ceiling is a property of the whole delivered file, so it applies to
  every channel *including* LFE (audit CPL-10): a -10 dB trim that skipped the
  LFE changed the mix balance, and a ceiling that skipped it let LFE overs
  through. These stages are gain-only (no spectral change), so the LFE still
  receives no *effect* processing.
* ``FRONT_PAIR_ONLY`` - stereo-image processing (M/S width, bass-mono, doubler).
  Only meaningful for the front L/R pair; centre, LFE, and surrounds pass
  through untouched.

``route_effect`` also enforces the channel contract: an effect must return
exactly the shape it was handed. That assertion is the load-bearing part of this
module. Two shipped effects (``_apply_bass_mono_maker``,
``_apply_mid_side_processor``) rebuilt their output with a hardcoded
two-channel stack and would have silently discarded C/LFE/Ls/Rs from a 5.1
render; the shape check turns that entire bug class into a loud, fail-closed
error on the first surround render instead of a truncated output file.

For mono and stereo sources the routed subset is the whole buffer, so this
module is a pass-through and existing render results cannot move.
"""
from __future__ import annotations

from enum import Enum
from typing import Callable

import numpy as np

from khensu_services.common.channel_layout import ChannelLayout


class Routing(Enum):
    PER_CHANNEL = "per_channel"
    ALL_CHANNELS = "all_channels"
    FRONT_PAIR_ONLY = "front_pair_only"


class ChannelContractError(RuntimeError):
    """An effect returned a different shape than it was given.

    Fail closed: a render that cannot preserve its channel contract must not
    write an output file.
    """


def _selected_indices(layout: ChannelLayout, routing: Routing) -> tuple[int, ...]:
    if routing is Routing.FRONT_PAIR_ONLY:
        left, right = layout.front_pair()
        # Mono folds both to channel 0; don't hand the effect a duplicated row.
        return (left,) if left == right else (left, right)
    if routing is Routing.ALL_CHANNELS:
        return tuple(range(layout.num_channels))
    return layout.non_lfe_indices()


def route_effect(
    fn: Callable[[np.ndarray], np.ndarray],
    audio: np.ndarray,
    layout: ChannelLayout,
    routing: Routing,
) -> np.ndarray:
    """Run ``fn`` over the channel subset ``routing`` selects and reassemble.

    ``audio`` is (channels, samples). Channels outside the selection are copied
    through unchanged. Raises ChannelContractError if ``fn`` does not return the
    same shape it was handed.
    """
    if audio.ndim != 2:
        # 1-D mono buffers have no channel structure to route; run as-is and
        # still enforce the shape contract.
        result = fn(audio)
        if np.shape(result) != np.shape(audio):
            raise ChannelContractError(
                f"Effect changed the buffer shape: expected {np.shape(audio)}, "
                f"got {np.shape(result)}."
            )
        return result

    indices = _selected_indices(layout, routing)
    sub = audio[list(indices), :]

    result = fn(sub)
    result = np.asarray(result)

    if result.shape != sub.shape:
        raise ChannelContractError(
            f"Effect returned {result.shape[0] if result.ndim > 1 else 1} channel(s) "
            f"x {result.shape[-1]} sample(s) but was given {sub.shape[0]} x "
            f"{sub.shape[1]} (layout {layout.name}, routing {routing.value}). "
            "Refusing to write a render that would drop or invent audio."
        )

    out = audio.copy()
    out[list(indices), :] = result
    return out
