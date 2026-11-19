# local-services/khensu_services/common/channel_layout.py
"""Channel-layout contract for the render path (audit AQ-01).

Python mirror of ``shared-core/include/khensu/analysis/ChannelLayout.h``. The
analysis half (C++) and the render half (Python) must agree on which layouts
exist, what order their channels are in, and which channel is LFE - so the
supported set and the SMPTE role order are declared once here and asserted
against the C++ contract in tests.

Layout is inferred from the channel count, exactly as on the C++ side: a
6-channel file is treated as 5.1 in standard order. That is a documented
assumption, not invented precision.
"""
from __future__ import annotations

from dataclasses import dataclass

# Channel count -> layout name. Anything not in this table is rejected.
SUPPORTED_LAYOUTS: dict[int, str] = {1: "Mono", 2: "Stereo", 6: "5.1", 8: "7.1"}

# SMPTE/WAV channel order per layout.
_ROLES: dict[int, tuple[str, ...]] = {
    1: ("C",),
    2: ("L", "R"),
    6: ("L", "R", "C", "LFE", "Ls", "Rs"),
    8: ("L", "R", "C", "LFE", "Ls", "Rs", "Lb", "Rb"),
}


class UnsupportedLayoutError(ValueError):
    """Raised when a file's channel count has no supported layout.

    Carries the same actionable message shape as the C++
    ``ChannelLayout::fromChannelCount`` error: the received count plus the
    supported set.
    """


@dataclass(frozen=True)
class ChannelLayout:
    name: str
    num_channels: int
    roles: tuple[str, ...]

    @staticmethod
    def from_channel_count(ch: int) -> "ChannelLayout":
        name = SUPPORTED_LAYOUTS.get(ch)
        if name is None:
            supported = ", ".join(
                f"{n} ({label})" for n, label in sorted(SUPPORTED_LAYOUTS.items())
            )
            raise UnsupportedLayoutError(
                f"Unsupported channel layout: {ch} channels. "
                f"Supported layouts are {supported}."
            )
        return ChannelLayout(name=name, num_channels=ch, roles=_ROLES[ch])

    def lfe_indices(self) -> tuple[int, ...]:
        return tuple(i for i, r in enumerate(self.roles) if r == "LFE")

    def non_lfe_indices(self) -> tuple[int, ...]:
        return tuple(i for i, r in enumerate(self.roles) if r != "LFE")

    def front_pair(self) -> tuple[int, int]:
        """Indices of the front L/R pair. Mono folds both onto channel 0."""
        return (0, 1) if self.num_channels > 1 else (0, 0)

    def has_lfe(self) -> bool:
        return bool(self.lfe_indices())
