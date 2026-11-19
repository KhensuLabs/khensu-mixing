# local-services/khensu_services/analysis/artifacts/spectral_tier.py
"""Spectral-tier detectors (spec §5.1, standard pass).

Statistical signatures that generation pipelines leave in the audio itself,
grounded in docs/Functionality_Research/ai-music-artifacts-humanization-report.md:

  SpectralCeilingDetector   — hard bandwidth ceiling from a neural
      vocoder/tokenizer (energy cliff around 12–17 kHz on a high-SR file).
      Honest lossless productions roll off smoothly; a >30 dB cliff at a
      fixed bin is a reconstruction artifact. Lossy encoders lowpass at
      fixed points too (MP3/AAC ~16 kHz at 128 kbps, ~19-20 kHz at high
      bitrates), so a cliff in those bands, or any cliff in a lossy
      container, is a codec fingerprint and is NOT reported (AUDIT-019).
  UniformNoiseFloorDetector — near-constant noise floor across the track.
      Real rooms/chains breathe; generated audio's floor variance is tiny.
  MacroDynamicsDetector     — verse/chorus/bridge at near-identical
      short-term level (low variance of frame RMS in dB across sections).
  SectionSimilarityDetector — copy-paste-identical sections via chroma
      self-similarity: repeated bars in human productions differ take-to-
      take; generated repeats can be bit-for-bit similar.

Audit 2026-11-06 (S1, AID-01/02): every one of these is something ordinary
human productions do all the time - a WAV decoded from an MP3, a limited
master, a looped chorus. They are therefore reported as *production-style
observations* (evidence_class "observation") with zero weight: context next to
the provenance evidence, never a driver of the verdict.
"""
from __future__ import annotations

import numpy as np

from khensu_services.analysis import _lean_spectral as _lean
from khensu_services.analysis.artifacts.base import ArtifactFinding, AudioContext


_LOSSY_EXTS = (".mp3", ".mp2", ".aac", ".m4a", ".mp4", ".ogg", ".oga", ".opus",
               ".wma", ".webm", ".amr", ".ac3")


def _is_lossy_container(file_path: str) -> bool:
    import os
    return os.path.splitext(file_path or "")[1].lower() in _LOSSY_EXTS


def _at_codec_cutoff(hz: float) -> bool:
    """Encoder low-pass region (AID-01): LAME's lowpass points are 17.0, 17.5,
    18.6 and 19.4 kHz, AAC/Vorbis sit anywhere in 15-20.5 kHz."""
    return 15000.0 <= hz <= 20500.0


class SpectralCeilingDetector:
    id = "spectral_ceiling"
    tier = "spectral"

    def scan(self, ctx: AudioContext) -> list[ArtifactFinding]:
        if ctx.sr < 32000:
            return []  # can't judge a ceiling below ~16 kHz of headroom
        if _is_lossy_container(ctx.file_path):
            return []  # every lossy encoder lowpasses; the cliff proves nothing
        # One lossy-origin detector shared with source forensics (AID-06):
        # steepest sustained cliff vs the level just below it.
        from khensu_services.analysis.source_forensics import (
            is_lossy_lowpass, lowpass_cliff_from_audio)
        cliff = lowpass_cliff_from_audio(ctx.mono, ctx.sr)
        if not cliff.get("detected") or cliff.get("cliff_hz") is None:
            return []
        cliff_hz = float(cliff["cliff_hz"])
        if is_lossy_lowpass(cliff) or _at_codec_cutoff(cliff_hz):
            return []  # lossy-encode lowpass (e.g. a WAV decoded from an MP3)
        return [ArtifactFinding(
            detector=self.id, kind="spectral_ceiling",
            summary=f"Hard band-limit near {cliff_hz / 1000:.1f} kHz",
            evidence=f"{float(cliff['drop_db']):.0f} dB sustained cliff at {cliff_hz:.0f} Hz "
                     f"(sr {ctx.sr}); a low-bitrate encode, a band-limited source or a "
                     "neural vocoder can all produce this",
            weight=0.5,
            services={},
            freq_range_hz=(cliff_hz, ctx.sr / 2),
            evidence_class="observation",
        )]


class UniformNoiseFloorDetector:
    id = "uniform_noise_floor"
    tier = "spectral"

    def scan(self, ctx: AudioContext) -> list[ArtifactFinding]:
        # Frame RMS in dB; the noise floor is the 5th percentile per block of
        # ~2 s. Variance of that floor across the track is the signal.
        frame = 2048
        n = len(ctx.mono) // frame
        if n < 30:
            return []
        rms = np.sqrt(np.mean(
            ctx.mono[: n * frame].reshape(n, frame) ** 2, axis=1))
        rms_db = 20.0 * np.log10(rms + 1e-9)
        block = max(10, n // 40)
        floors = [np.percentile(rms_db[i:i + block], 5)
                  for i in range(0, n - block, block)]
        if len(floors) < 8:
            return []
        spread = float(np.std(floors))
        floor_mean = float(np.mean(floors))
        # Real material's quiet-floor wanders by several dB; require an
        # audible floor (not digital silence) that barely moves.
        if spread < 1.0 and floor_mean > -70.0:
            return [ArtifactFinding(
                detector=self.id, kind="uniform_noise_floor",
                summary="Noise floor is constant across the track",
                evidence=f"floor {floor_mean:.1f} dBFS, std {spread:.2f} dB "
                         "across the song (common on limited or programmed productions)",
                weight=0.35,
                evidence_class="observation",
            )]
        return []


class MacroDynamicsDetector:
    id = "macro_dynamics"
    tier = "spectral"

    def scan(self, ctx: AudioContext) -> list[ArtifactFinding]:
        # 3-second loudness frames; std in dB measures section contrast.
        win = ctx.sr * 3
        n = len(ctx.mono) // win
        if n < 8:
            return []
        rms = np.sqrt(np.mean(
            ctx.mono[: n * win].reshape(n, win) ** 2, axis=1))
        rms_db = 20.0 * np.log10(rms + 1e-9)
        active = rms_db[rms_db > rms_db.max() - 30.0]   # ignore silence/tails
        if len(active) < 8:
            return []
        spread = float(np.std(active))
        if spread < 1.2:
            return [ArtifactFinding(
                detector=self.id, kind="macro_dynamics",
                summary="Sections sit at near-identical loudness (no macro-dynamics)",
                evidence=f"3s-frame level std {spread:.2f} dB over "
                         f"{len(active)} active frames (typical of a heavily limited master)",
                weight=0.3,
                evidence_class="observation",
            )]
        return []


class SectionSimilarityDetector:
    id = "section_similarity"
    tier = "spectral"

    def scan(self, ctx: AudioContext) -> list[ArtifactFinding]:
        if len(ctx.mono) < ctx.sr * 30:
            return []
        chroma = _lean.chroma_stft(np.asarray(ctx.mono), ctx.sr, hop_length=4096)
        # 4-second chroma blocks, pairwise correlation at long lags.
        block = max(1, int(4.0 * ctx.sr / 4096))
        nb = chroma.shape[1] // block
        if nb < 8:
            return []
        blocks = np.stack([
            chroma[:, i * block:(i + 1) * block].mean(axis=1) for i in range(nb)])
        blocks = blocks / (np.linalg.norm(blocks, axis=1, keepdims=True) + 1e-9)
        sim = blocks @ blocks.T
        # Off-diagonal (lag >= 4 blocks = 16 s apart) near-duplicates.
        pairs = [(i, j) for i in range(nb) for j in range(i + 4, nb)
                 if sim[i, j] > 0.999]
        if len(pairs) >= 3:
            i, j = pairs[0]
            return [ArtifactFinding(
                detector=self.id, kind="section_similarity",
                summary="Distant sections are near-identical (copied or looped)",
                evidence=f"{len(pairs)} block pairs with chroma correlation "
                         "> 0.999 at 16+ second separation (copy-paste arrangement, "
                         "loops or samples)",
                weight=0.3,
                evidence_class="observation",
                time_range_sec=(i * 4.0, (j + 1) * 4.0),
            )]
        return []
