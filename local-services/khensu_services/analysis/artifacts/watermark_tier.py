# local-services/khensu_services/analysis/artifacts/watermark_tier.py
"""Watermark-presence detection (spec §5.1).

Reports the *presence* of a SynthID-class provenance watermark; it does not
decode or remove it. SynthID (Google/Lyria) embeds an imperceptible pattern
in the spectrogram that survives noise, MP3 and time-stretch — engineered to
be statistically detectable while inaudible. Without Google's key we cannot
read the payload, but the class of signal leaves a measurable footprint: a
faint, unusually *stationary* structured energy pattern in the upper-mid /
high bands whose short-time statistics are more uniform than natural
program material produces.

Audit 2026-11-06 AID-03: sustained harmonic music (an organ chord, a pad)
also keeps a high, stationary floor in its partials, and the detector used to
name Lyria/SynthID at 0.55 for it. Two changes: tonal partials are excluded
(bins that stand well above their spectral neighbourhood, plus their skirts),
and the band must be predominantly noise-like before the stationarity is even
measured. A hit is an *undecoded heuristic*: it is reported as a production-
style observation with zero weight and never names a vendor (at most "other"
at 0.2) - a vendor can only be named from a decoded watermark or metadata.

Scope (spec §5.2): detection only. There is intentionally no removal path.
"""
from __future__ import annotations

import numpy as np

from khensu_services.analysis.artifacts.base import ArtifactFinding, AudioContext


class WatermarkPresenceDetector:
    id = "watermark_presence"
    tier = "watermark"

    # Band where imperceptible spectrogram watermarks are typically carried:
    # high enough to be masked, low enough to survive lossy codecs.
    _LO_HZ = 6000.0
    _HI_HZ = 14000.0

    def scan(self, ctx: AudioContext) -> list[ArtifactFinding]:
        if ctx.sr < 30000 or len(ctx.mono) < ctx.sr * 5:
            return []
        import librosa

        n_fft = 2048
        hop = n_fft // 4
        S = ctx.abs_stft(n_fft, hop)   # shared with codec-residual (BE-07)
        freqs = librosa.fft_frequencies(sr=ctx.sr, n_fft=n_fft)
        band = (freqs >= self._LO_HZ) & (freqs <= self._HI_HZ)
        if not band.any():
            return []

        B = S[band]                       # (bins, frames)
        if B.shape[1] < 40:
            return []

        bin_energy = B ** 2
        med = np.median(bin_energy, axis=1) + 1e-12

        # Exclude tonal partials (AID-03): a bin whose median energy stands
        # > 6 dB above the median of its +-12-bin neighbourhood is a partial;
        # its +-2-bin window skirt goes with it.
        from scipy.ndimage import maximum_filter1d, median_filter
        local = median_filter(med, size=25, mode="nearest") + 1e-12
        tonal = med > 4.0 * local
        tonal = maximum_filter1d(tonal.astype(np.uint8), size=5) > 0
        noise_like = ~tonal
        tonal_share = float(med[tonal].sum() / med.sum()) if tonal.any() else 0.0
        if tonal_share > 0.5 or noise_like.sum() < 16:
            return []   # the band is carried by partials (harmonic music), not a carrier

        # Per-bin energy over time on the noise-like bins only; a data-bearing
        # carrier keeps a faint but persistent floor where natural material
        # falls towards silence between events. Ratio of the 10th percentile
        # (the "floor") to the median - natural bins have a deep floor (~0).
        # Energy is smoothed over 4 frames (~46 ms) first: a noise-like
        # carrier's per-frame energy is exponentially distributed, so its raw
        # 10th-percentile/median ratio sits at ~0.15 however stationary it is
        # (smoothed: ~0.4); material that breathes between events stays ~0.1.
        from scipy.ndimage import uniform_filter1d
        nb = uniform_filter1d(bin_energy[noise_like], size=4, axis=1, mode="nearest")
        nmed = np.median(nb, axis=1) + 1e-12
        floor = np.percentile(nb, 10, axis=1)
        floor_ratio = floor / nmed
        active = nmed > (np.median(nmed) * 0.05)
        if active.sum() < 8:
            return []
        stationarity = float(np.median(floor_ratio[active]))

        # Total band energy must be non-trivial (not just codec noise).
        band_share = float(bin_energy.sum() / (S ** 2).sum() + 1e-12)

        if stationarity > 0.25 and band_share > 5e-4:
            return [ArtifactFinding(
                detector=self.id, kind="watermark_presence",
                summary="Stationary high-band energy (possible watermark carrier; not decoded)",
                evidence=f"noise-like {int(self._LO_HZ/1000)}-{int(self._HI_HZ/1000)} kHz bins keep "
                         f"a persistent floor: floor/median ratio {stationarity:.2f} "
                         f"(band share {band_share*100:.2f}%, tonal share {tonal_share*100:.0f}%). "
                         "Heuristic presence only - payload not decoded, so no source is "
                         "named; removal out of scope.",
                # Signal strength only; as an observation it carries no
                # verdict weight (the engine's effective weight is 0).
                weight=float(np.clip(0.25 + (stationarity - 0.25) * 1.5, 0.25, 0.55)),
                services={"other": 0.2},
                freq_range_hz=(self._LO_HZ, self._HI_HZ),
                evidence_class="observation",
            )]
        return []
